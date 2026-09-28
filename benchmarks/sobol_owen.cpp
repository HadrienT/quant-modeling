// Owen's scrambling against the digital shift, on every library script
// (blueprint/wp/19-gpu.md §12): does Owen pay, and where?
//
//   build-cuda/qm_gpu_owen_bench [seeds=200]
//
// Sobol RQMC + Brownian bridge, 16 replicates, priced on the GPU: local vol
// (a skewed surface, 52 steps a year) for one underlying, correlated
// Black-Scholes for several. For each script, hard and fuzzy, at 16 x 2^12
// and 16 x 2^15 points: the *true* spread of the price over `seeds`
// independent seeds with each randomisation, their variance ratio (shift /
// Owen: above 1, Owen is better) and its 95 % confidence interval -- the
// log of a ratio of two sample variances on n - 1 degrees of freedom each is
// ~ N(log R, 4 / (n - 1)) -- and whether the script has a hard spot
// threshold (ScriptAnalysis::spot_threshold_test), the candidate rule.

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
    double spread(const std::vector<double> &x)
    {
        double m = 0.0, v = 0.0;
        for (double a : x)
            m += a / static_cast<double>(x.size());
        for (double a : x)
            v += (a - m) * (a - m) / static_cast<double>(x.size() - 1);
        return v;
    }
} // namespace

int main(int argc, char **argv)
{
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 200;
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

    const double ci = std::exp(1.96 * std::sqrt(4.0 / (seeds - 1)));
    std::printf("Owen vs digital shift, Sobol + bridge, 16 replicates, %d seeds; ratio = var(shift) / var(owen),\n"
                "95 %% interval ratio x/ %.2f. threshold: the script tests a spot against a level.\n\n",
                seeds, ci);
    std::printf("%-30s %-5s %-9s %9s %8s %8s  %s\n", "script", "mode", "threshold", "points", "ratio", "95% low",
                "verdict");
    for (const auto &path : files)
    {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        for (const bool fuzzy : {false, true})
        {
            qm::ScriptSettings ss;
            ss.fuzzy = fuzzy;
            const qm::ScriptedProduct<Real> product(text.str(), ctx, ss);
            const std::size_t n = product.n_underlyings();
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
                std::vector<double> est[2];
                for (int owen = 0; owen < 2; ++owen)
                    for (int s = 1; s <= seeds; ++s)
                    {
                        qm::PricingSettings set;
                        set.mc_paths = points;
                        set.mc_seed = s;
                        set.mc_sampler = qm::SamplerKind::Sobol;
                        set.mc_sobol_owen = owen == 1;
                        set.mc_device = qm::ComputeDevice::Gpu;
                        auto m = make();
                        est[owen].push_back(qm::simulate_script(product, *m, set).npv());
                    }
                const double v0 = spread(est[0]), v1 = spread(est[1]);
                const double ratio = v1 > 0.0 ? v0 / v1 : 0.0;
                const char *verdict = v0 == 0.0 && v1 == 0.0 ? "no variance"
                                      : ratio / ci > 1.0     ? "OWEN BETTER"
                                      : ratio * ci < 1.0     ? "owen worse"
                                                             : "indistinguishable";
                std::printf("%-30s %-5s %-9s %9d %8.2f %8.2f  %s\n", path.stem().string().c_str(),
                            fuzzy ? "fuzzy" : "hard", product.analysis().spot_threshold_test ? "yes" : "no", points,
                            ratio, ratio / ci, verdict);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
