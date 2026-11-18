// How much of Sobol's edge a discontinuous payoff loses, and how much the
// smoothed indicators give back (blueprint/wp/26-performance.md, P1c
// "Quality", issue #226 item 3).
//
//   build-cuda/qm_gpu_discontinuous_bench [seeds=200]
//
// Every library script (those with exercise() left out: the GPU needs a
// fitted rule), local vol (the skewed surface of qm_gpu_owen_bench, 52
// steps a year) for one underlying, correlated Black-Scholes for several,
// on the GPU, 16 replicates, at 2^16 and 2^19 points. For each of
// {pseudo-random, Sobol + bridge} x {hard, fuzzy}, the true spread of the
// price over `seeds` independent seeds. Columns:
//
//   qmc hard   var(pseudo, hard) / var(Sobol, hard): Sobol's edge as written;
//   qmc fuzzy  var(pseudo, fuzzy) / var(Sobol, fuzzy): its edge once smoothed;
//   smooth     var(Sobol, hard) / var(Sobol, fuzzy): what smoothing gives a
//              Sobol run (the decision this bench is for);
//   bias       (mean fuzzy - mean hard) / sd of one hard Sobol run: the
//              smoothing's bias in units of the error a run shows (both
//              means over the seeds, so their own error is sd / sqrt(seeds)).
//
// A ratio's 95 % interval is x/ exp(1.96 sqrt(4 / (seeds - 1))) (1.32 at 200
// seeds), as in qm_gpu_owen_bench.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"

namespace qm = quantModeling;
using qm::Real;

namespace
{
    struct Moments
    {
        double mean = 0.0, var = 0.0;
    };

    Moments moments(const std::vector<double> &x)
    {
        Moments m;
        for (double a : x)
            m.mean += a / static_cast<double>(x.size());
        for (double a : x)
            m.var += (a - m.mean) * (a - m.mean) / static_cast<double>(x.size() - 1);
        return m;
    }

    double ratio(double num, double den)
    {
        return den > 0.0 ? num / den : 0.0;
    }
} // namespace

int main(int argc, char **argv)
{
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 200;
    const char *only = std::getenv("QM_BENCH_ONLY");
    if (qm::gpu::device_count() == 0)
    {
        std::fprintf(stderr, "no CUDA device\n");
        return 1;
    }
    for (int d = 0; d < qm::gpu::device_count(); ++d)
        qm::gpu::warm_up(d);

    const qm::ValuationContext ctx{qm::Date::from_iso("2026-06-01")};
    const std::vector<Real> K{60, 80, 90, 100, 110, 120, 150}, T{0.25, 0.5, 1.0, 2.0, 5.0};
    std::vector<Real> grid;
    for (Real k : K)
        for (Real t : T)
            grid.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);

    std::vector<std::filesystem::path> files;
    for (const auto &e : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
        if (e.path().extension() == ".qms")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    std::printf("Discontinuous payoffs under Sobol, 16 replicates, %d seeds; ratio intervals x/ %.2f.\n"
                "threshold: the script tests a spot against a level.\n\n",
                seeds, std::exp(1.96 * std::sqrt(4.0 / (seeds - 1))));
    std::printf("%-30s %-9s %9s %9s %9s %9s %8s\n", "script", "threshold", "points", "qmc hard", "qmc fuzzy",
                "smooth", "bias");
    for (const auto &path : files)
    {
        if (only && path.stem().string().find(only) == std::string::npos)
            continue;
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        qm::ScriptSettings hard_settings, fuzzy_settings;
        fuzzy_settings.fuzzy = true;
        const qm::ScriptedProduct<Real> hard(text.str(), ctx, hard_settings);
        const qm::ScriptedProduct<Real> fuzzy(text.str(), ctx, fuzzy_settings);
        if (hard.has_exercise())
        {
            std::printf("%-30s skipped: early exercise\n", path.stem().string().c_str());
            continue;
        }
        const std::size_t n = hard.n_underlyings();
        auto make = [&]() -> std::unique_ptr<qm::ISimulationModel<Real>>
        {
            if (n > 1)
            {
                Eigen::MatrixXd c = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n),
                                                              static_cast<Eigen::Index>(n), 0.5);
                c.diagonal().setOnes();
                return std::make_unique<qm::MultiAssetBSSimModel<Real>>(
                    std::vector<Real>(n, 100.0), 0.03, std::vector<Real>(n, 0.01), std::vector<Real>(n, 0.25), c);
            }
            return std::make_unique<qm::LocalVolSimModel<Real>>(100.0, 0.03, 0.01, K, T, grid, 1.0 / 52.0);
        };
        for (const int points : {16 << 12, 16 << 15})
        {
            // [sobol][fuzzy]
            Moments m[2][2];
            for (int sobol = 0; sobol < 2; ++sobol)
                for (int smooth = 0; smooth < 2; ++smooth)
                {
                    std::vector<double> est;
                    for (int s = 1; s <= seeds; ++s)
                    {
                        qm::PricingSettings set;
                        set.mc_paths = points;
                        set.mc_seed = s;
                        set.mc_sampler = sobol ? qm::SamplerKind::Sobol : qm::SamplerKind::PseudoRandom;
                        set.mc_rng = qm::RngKind::Philox;
                        set.mc_antithetic = false;
                        set.mc_device = qm::ComputeDevice::Gpu;
                        auto model = make();
                        est.push_back(qm::simulate_script(smooth ? fuzzy : hard, *model, set).npv());
                    }
                    m[sobol][smooth] = moments(est);
                }
            const double sd = std::sqrt(m[1][0].var);
            std::printf("%-30s %-9s %9d %9.2f %9.2f %9.2f %8.2f\n", path.stem().string().c_str(),
                        hard.analysis().spot_threshold_test ? "yes" : "no", points, ratio(m[0][0].var, m[1][0].var),
                        ratio(m[0][1].var, m[1][1].var), ratio(m[1][0].var, m[1][1].var),
                        sd > 0.0 ? (m[1][1].mean - m[1][0].mean) / sd : 0.0);
            std::fflush(stdout);
        }
    }
    return 0;
}
