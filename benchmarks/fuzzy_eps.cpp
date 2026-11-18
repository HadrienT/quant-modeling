// The smoothing width of fuzzy scripts: what each eps costs the price (its
// bias) and buys the adjoint delta (its bias and its noise), on every library
// script that tests a spot against a level (blueprint/wp/26-performance.md,
// P1c "Quality", issue #226 follow-up).
//
//   build-cuda/qm_gpu_fuzzy_eps_bench [seeds=16]
//
// The protocol of the other Sobol benches: local vol (52 steps a year) for
// one underlying, correlated Black-Scholes for several, on the GPU, Sobol +
// bridge, 16 replicates, 2^16 points (the site's 50 000 paths, rounded).
//
//   reference delta  central difference of the hard price at the spot of
//                    underlying 0 +- 1 %, the same draws on both sides,
//                    2^18 points, averaged over `seeds` seeds;
//   price bias       (mean fuzzy price - mean hard price) / sd of one hard
//                    run: 1 is the error the page shows;
//   delta bias       (mean adjoint delta - reference) / |reference|;
//   delta noise      the adjoint delta's own standard error / |reference|;
//   rmse             sqrt(bias^2 + noise^2): what one run's delta misses.
//
// eps is the ramp's half-width in the units of the compared expression (a
// performance ~1, a spot ~100): the grid spans both.

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
    double mean(const std::vector<double> &x)
    {
        double m = 0.0;
        for (double a : x)
            m += a / static_cast<double>(x.size());
        return m;
    }
    double sd(const std::vector<double> &x)
    {
        const double m = mean(x);
        double v = 0.0;
        for (double a : x)
            v += (a - m) * (a - m) / static_cast<double>(x.size() - 1);
        return std::sqrt(v);
    }
} // namespace

int main(int argc, char **argv)
{
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 16;
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
    const std::vector<double> eps_grid{0.001, 0.003, 0.01, 0.03, 0.1, 0.3, 1.0, 3.0};
    const int points = 16 << 12, ref_points = 16 << 14;
    const double h = 0.01;

    std::vector<std::filesystem::path> files;
    for (const auto &e : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
        if (e.path().extension() == ".qms")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    std::printf("Fuzzy eps: price bias (sd of one hard run), adjoint delta bias / noise / rmse (%% of the "
                "reference), %d seeds, %d points.\n\n",
                seeds, points);
    std::printf("%-30s %8s %10s %10s %10s %10s %12s\n", "script", "eps", "price bias", "delta bias", "noise",
                "rmse", "ref delta");
    for (const auto &path : files)
    {
        if (only && path.stem().string().find(only) == std::string::npos)
            continue;
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        const qm::ScriptedProduct<Real> hard(text.str(), ctx);
        if (hard.has_exercise() || !hard.analysis().spot_threshold_test)
            continue;
        const std::size_t n = hard.n_underlyings();
        auto make = [&](double bump) -> std::unique_ptr<qm::ISimulationModel<Real>>
        {
            if (n > 1)
            {
                Eigen::MatrixXd c = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n),
                                                              static_cast<Eigen::Index>(n), 0.5);
                c.diagonal().setOnes();
                std::vector<Real> s(n, 100.0);
                s[0] *= bump;
                return std::make_unique<qm::MultiAssetBSSimModel<Real>>(
                    s, 0.03, std::vector<Real>(n, 0.01), std::vector<Real>(n, 0.25), c);
            }
            return std::make_unique<qm::LocalVolSimModel<Real>>(100.0 * bump, 0.03, 0.01, K, T, grid, 1.0 / 52.0);
        };
        auto price = [&](const qm::ScriptedProduct<Real> &p, double bump, int s, int pts)
        {
            qm::PricingSettings set;
            set.mc_paths = pts;
            set.mc_seed = s;
            set.mc_sampler = qm::SamplerKind::Sobol;
            set.mc_device = qm::ComputeDevice::Gpu;
            auto m = make(bump);
            return qm::simulate_script(p, *m, set).npv();
        };

        std::vector<double> fd, hard_price;
        for (int s = 1; s <= seeds; ++s)
        {
            fd.push_back((price(hard, 1 + h, s, ref_points) - price(hard, 1 - h, s, ref_points)) / (2 * h * 100.0));
            hard_price.push_back(price(hard, 1.0, s, points));
        }
        const double ref = mean(fd), ref_se = sd(fd) / std::sqrt(seeds);
        const double hard_sd = sd(hard_price), hard_mean = mean(hard_price);
        if (std::abs(ref) < 1e-12)
        {
            std::printf("%-30s skipped: no delta\n", path.stem().string().c_str());
            continue;
        }
        for (const double eps : eps_grid)
        {
            qm::ScriptSettings ss;
            ss.fuzzy = true;
            ss.default_eps = eps;
            const qm::ScriptedProduct<Real> fuzzy(text.str(), ctx, ss);
            std::vector<double> p, d, se;
            std::string why;
            for (int s = 1; s <= seeds; ++s)
            {
                auto m = make(1.0);
                const auto r = qm::simulate_script_aad_gpu(fuzzy, *m, static_cast<std::size_t>(points),
                                                           static_cast<std::uint64_t>(s), why, qm::SamplerKind::Sobol);
                if (!r)
                    break;
                std::size_t k = 0;
                while (k < r->risk_labels.size() && r->risk_labels[k].rfind("spot", 0) != 0)
                    ++k;
                p.push_back(r->price);
                d.push_back(k < r->risks.size() ? r->risks[k] : 0.0);
                se.push_back(k < r->risks.size() ? r->risk_std_errors[k] : 0.0);
            }
            if (p.size() < 2)
            {
                std::printf("%-30s %8.3f  no GPU adjoint: %s\n", path.stem().string().c_str(), eps, why.c_str());
                break;
            }
            const double bias = (mean(d) - ref) / std::abs(ref), noise = mean(se) / std::abs(ref);
            std::printf("%-30s %8.3f %10.2f %9.1f%% %9.1f%% %9.1f%% %8.4f+-%.4f\n", path.stem().string().c_str(),
                        eps, hard_sd > 0 ? (mean(p) - hard_mean) / hard_sd : 0.0, 100 * bias, 100 * noise,
                        100 * std::hypot(bias, noise), ref, ref_se);
            std::fflush(stdout);
        }
    }
    return 0;
}
