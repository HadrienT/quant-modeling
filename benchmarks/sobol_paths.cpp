// Principal components against the Brownian bridge for building Sobol's
// paths, on every library script (blueprint/wp/26-performance.md P1c
// "Quality", issue #226 item 2).
//
//   build-cuda/qm_gpu_paths_bench [seeds=200]
//
// The protocol of qm_gpu_owen_bench: local vol (52 steps a year) for one
// underlying, correlated Black-Scholes for several, on the GPU, Sobol with
// the digital shift, 16 replicates, at 2^16 and 2^19 points, hard; the true
// spread over `seeds` seeds with each construction, their variance ratio
// (bridge / PCA: above 1, PCA is better) and its 95 % interval, and the
// wall time of one run with each (PCA is O(n^2) a path and a factor, the
// bridge O(n)). Scripts with exercise() are left out (no fitted rule).

#include <algorithm>
#include <chrono>
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

    const double ci = std::exp(1.96 * std::sqrt(4.0 / (seeds - 1)));
    std::printf("PCA vs Brownian bridge, Sobol + digital shift, 16 replicates, %d seeds; ratio = var(bridge) / "
                "var(pca), 95 %% interval ratio x/ %.2f; cost = time(pca) / time(bridge) of one run.\n\n",
                seeds, ci);
    std::printf("%-30s %-9s %9s %8s %8s %6s  %s\n", "script", "threshold", "points", "ratio", "95% low", "cost",
                "verdict");
    for (const auto &path : files)
    {
        if (only && path.stem().string().find(only) == std::string::npos)
            continue;
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        for (const bool fuzzy : {false})
        {
            qm::ScriptSettings ss;
            ss.fuzzy = fuzzy;
            const qm::ScriptedProduct<Real> product(text.str(), ctx, ss);
            // early exercise needs a fitted rule, which this protocol does not
            // fit (the GPU refuses the script without one): left out, said
            if (product.has_exercise())
            {
                if (!fuzzy)
                    std::printf("%-30s skipped: early exercise\n", path.stem().string().c_str());
                continue;
            }
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
                double seconds[2] = {0.0, 0.0};
                for (int pca = 0; pca < 2; ++pca)
                    for (int s = 1; s <= seeds; ++s)
                    {
                        qm::PricingSettings set;
                        set.mc_paths = points;
                        set.mc_seed = s;
                        set.mc_sampler = qm::SamplerKind::Sobol;
                        set.mc_brownian_pca = pca == 1;
                        set.mc_device = qm::ComputeDevice::Gpu;
                        auto m = make();
                        const auto t0 = std::chrono::steady_clock::now();
                        est[pca].push_back(qm::simulate_script(product, *m, set).npv());
                        seconds[pca] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    }
                const double v0 = spread(est[0]), v1 = spread(est[1]);
                const double ratio = v1 > 0.0 ? v0 / v1 : 0.0;
                const char *verdict = v0 == 0.0 && v1 == 0.0 ? "no variance"
                                      : ratio / ci > 1.0     ? "PCA BETTER"
                                      : ratio * ci < 1.0     ? "pca worse"
                                                             : "indistinguishable";
                std::printf("%-30s %-9s %9d %8.2f %8.2f %6.2f  %s\n", path.stem().string().c_str(),
                            product.analysis().spot_threshold_test ? "yes" : "no", points, ratio, ratio / ci,
                            seconds[0] > 0.0 ? seconds[1] / seconds[0] : 0.0, verdict);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
