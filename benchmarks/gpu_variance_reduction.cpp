// Benchmark of blueprint/wp/19-gpu.md §2.5 (lot G3): what the generic
// variance reduction buys, measured, on library scripts priced on the GPU
// under a skewed local vol.
//
//   build-cuda/qm_gpu_vr_bench [paths=100000] [seeds=40]
//
// For each script and technique -- plain, stratified terminal value, spot
// controls, both, Sobol RQMC with the Brownian bridge (no mirror: its two
// rows are the same run), importance sampling (GHS drift, kept only when its
// pilot pays; its time includes the drift search and the pilot) -- with and
// without antithetic pairs: the *true* spread of
// the price over independent seeds (not the estimate a single run reports),
// the mean reported standard error beside it, the variance ratio to the
// plain run at the same number of paths (from either), and the GPU time per
// run -- so a reduction is read as time to a given error, not as a bare
// ratio.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"

namespace qm = quantModeling;

namespace
{
    std::string read_script(const std::string &name)
    {
        std::ifstream in(std::filesystem::path(QM_PRODUCT_LIBRARY_DIR) / (name + ".qms"));
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    struct Row
    {
        double true_sd = 0.0, mean_se = 0.0, ms = 0.0;
    };
} // namespace

int main(int argc, char **argv)
{
    const int paths = argc > 1 ? std::atoi(argv[1]) : 100000;
    const int seeds = argc > 2 ? std::atoi(argv[2]) : 40;
    if (qm::gpu::device_count() == 0)
    {
        std::fprintf(stderr, "no CUDA device\n");
        return 1;
    }
    qm::gpu::warm_up(0);

    const qm::ValuationContext ctx{qm::Date::from_iso("2026-06-01")};
    const std::vector<qm::Real> K{60, 80, 90, 100, 110, 120, 150}, T{0.25, 0.5, 1.0, 2.0, 5.0};
    std::vector<qm::Real> grid;
    for (qm::Real k : K)
        for (qm::Real t : T)
            grid.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
    const std::vector<qm::Real> flat(grid.size(), 0.25);

    struct Technique
    {
        const char *name;
        qm::SamplerKind sampler;
        bool control;
        bool importance = false;
    };
    const Technique techniques[] = {{"plain", qm::SamplerKind::PseudoRandom, false},
                                    {"stratified", qm::SamplerKind::Stratified, false},
                                    {"control", qm::SamplerKind::PseudoRandom, true},
                                    {"both", qm::SamplerKind::Stratified, true},
                                    {"sobol", qm::SamplerKind::Sobol, false},
                                    {"importance", qm::SamplerKind::PseudoRandom, false, true}};

    std::printf("%d paths per run, %d seeds, local vol (52 steps/yr), GPU %s\n\n", paths, seeds,
                qm::gpu::device_name(0).c_str());
    std::printf("%-18s %-5s %-11s %10s %10s %8s %8s %8s %8s\n", "script", "anti", "technique", "true sd", "mean se",
                "var / sd", "var / se", "ms/run", "speed-up");
    // A rare event beside the library: a one-year digital paying 100 above
    // 180 % of the spot (~0.8 % likely at 25 % vol) -- importance sampling's
    // case.
    const std::string deep_digital = "2027-06-01\n    if spot() > 180 then pays 100 endIf\n";
    for (const std::string name : {"european-call", "asian-call", "phoenix-autocall", "up-and-out-call",
                                   "worst-of-autocall", "variance-swap", "deep-digital"})
    {
        const qm::ScriptedProduct<qm::Real> product(name == "deep-digital" ? deep_digital : read_script(name), ctx);
        if (product.n_underlyings() > 1)
            continue;
        double default_cost = 0.0; // se^2 x time of the default: antithetic, nothing else
        for (const bool anti : {true, false})
        {
            double plain_var = 0.0, plain_se = 0.0;
            for (const Technique &tech : techniques)
            {
                std::vector<double> v;
                Row row;
                for (int s = 1; s <= seeds; ++s)
                {
                    qm::PricingSettings set;
                    set.mc_paths = paths;
                    set.mc_seed = s;
                    set.mc_antithetic = anti;
                    set.mc_rng = qm::RngKind::Philox;
                    set.mc_device = qm::ComputeDevice::Gpu;
                    set.mc_sampler = tech.sampler;
                    set.mc_spot_control = tech.control;
                    set.mc_importance_drift = tech.importance;
                    // the rare event under a flat 25 % surface: under the skew, 180 is out of reach
                    qm::LocalVolSimModel<qm::Real> model(100.0, 0.03, 0.01, K, T,
                                                         name == "deep-digital" ? flat : grid, 1.0 / 52.0);
                    const auto t0 = std::chrono::steady_clock::now();
                    const qm::SimulationMCResult r = qm::simulate_script(product, model, set);
                    row.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    v.push_back(r.npv());
                    row.mean_se += r.std_error();
                }
                double m = 0.0, var = 0.0;
                for (double x : v)
                    m += x / seeds;
                for (double x : v)
                    var += (x - m) * (x - m) / (seeds - 1);
                row.true_sd = std::sqrt(var);
                row.mean_se /= seeds;
                row.ms /= seeds;
                if (tech.sampler == qm::SamplerKind::PseudoRandom && !tech.control && !tech.importance)
                {
                    plain_var = var;
                    plain_se = row.mean_se;
                    if (anti)
                        default_cost = row.mean_se * row.mean_se * row.ms;
                }
                std::printf("%-18s %-5s %-11s %10.5f %10.5f %8.2f %8.2f %8.1f %8.2f\n", name.c_str(),
                            anti ? "yes" : "no", tech.name, row.true_sd, row.mean_se, plain_var / var,
                            (plain_se * plain_se) / (row.mean_se * row.mean_se), row.ms,
                            default_cost / (row.mean_se * row.mean_se * row.ms));
            }
        }
    }
    std::printf("\nvar / sd: variance of the plain run over the technique's, from the spread of the price over\n"
                "the seeds (itself uncertain by ~%.0f %% with %d seeds). var / se: the same ratio from the\n"
                "standard errors the runs report, averaged over the seeds (much tighter). speed-up: time to\n"
                "reach a given error against the default run (antithetic only), se^2 x time.\n",
                100.0 * std::sqrt(2.0 / (seeds - 1)) * 2.0, seeds);
    return 0;
}
