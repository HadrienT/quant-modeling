// Benchmark of blueprint/wp/19-gpu.md §6 and §9 (lot G3): a script's model
// risks on one V100 against the CPU tape on one thread, on the same Philox
// paths -- so both columns compute the same numbers, and the largest gap
// between the two risk vectors is printed beside the times.
//
//   build-cuda/qm_gpu_risk_bench [paths=100000]
//
// Local vol (the superbucket's input: dV/dsigma_loc on a 30 x 12 Dupire
// grid, the market_vega endpoint's size), by the per-path adjoint; Black-
// Scholes and Heston by forward-mode duals.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
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

    double ms_since(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // A 30 x 12 grid as the market_vega endpoint builds it (strikes 70..140).
    std::vector<qm::Real> strikes()
    {
        std::vector<qm::Real> k;
        for (int i = 0; i < 30; ++i)
            k.push_back(70.0 + 70.0 * i / 29.0);
        return k;
    }
    std::vector<qm::Real> maturities()
    {
        std::vector<qm::Real> t;
        for (int j = 0; j < 12; ++j)
            t.push_back(0.1 + 2.9 * j / 11.0);
        return t;
    }
    template <class T>
    std::vector<T> sigma_loc()
    {
        std::vector<T> g;
        for (qm::Real k : strikes())
            for (qm::Real t : maturities())
                g.push_back(T(0.2 + 0.25 * (100.0 - k) / 100.0 + 0.01 * t));
        return g;
    }

    template <class T>
    std::unique_ptr<qm::ISimulationModel<T>> make(const std::string &model)
    {
        if (model == "local_vol")
            return std::make_unique<qm::LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.01), strikes(), maturities(),
                                                             sigma_loc<T>(), 1.0 / 252.0);
        if (model == "heston")
            return std::make_unique<qm::BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05),
                                                          T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0), 1.0 / 252.0);
        return std::make_unique<qm::BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25));
    }
} // namespace

int main(int argc, char **argv)
{
    const std::size_t paths = argc > 1 ? static_cast<std::size_t>(std::atol(argv[1])) : 100000;
    if (qm::gpu::device_count() == 0)
    {
        std::fprintf(stderr, "no CUDA device\n");
        return 1;
    }
    qm::gpu::warm_up(0);
    const qm::ValuationContext ctx{qm::Date::from_iso("2026-06-01")};
    const std::uint64_t seed = 7;

    std::printf("%zu paths, Philox, one CPU thread (tape) against one %s\n\n", paths,
                qm::gpu::device_name(0).c_str());
    std::printf("%-18s %-10s %7s %11s %11s %9s %12s\n", "script", "model", "risks", "CPU tape ms", "GPU ms",
                "speed-up", "max |gap|");
    const std::pair<const char *, const char *> cases[] = {
        {"european-call", "local_vol"}, {"up-and-out-call", "local_vol"}, {"phoenix-autocall", "local_vol"}, {"variance-swap", "local_vol"}, {"phoenix-autocall", "black_scholes"}, {"phoenix-autocall", "heston"}, {"up-and-out-call", "heston"}};
    for (const auto &[name, model] : cases)
    {
        const std::string src = read_script(name);
        const qm::ScriptedProduct<qm::Real> product(src, ctx);
        auto gpu_model = make<qm::Real>(model);
        std::string why;
        auto t0 = std::chrono::steady_clock::now();
        const auto gpu = qm::simulate_script_aad_gpu(product, *gpu_model, paths, seed, why);
        const double gpu_ms = ms_since(t0);
        if (!gpu)
        {
            std::printf("%-18s %-10s  (GPU: %s)\n", name, model, why.c_str());
            continue;
        }

        const qm::ScriptedProduct<qm::aad::Number> tape_product(src, ctx);
        auto tape_model = make<qm::aad::Number>(model);
        t0 = std::chrono::steady_clock::now();
        const qm::AADSimulResults cpu = qm::simulate_aad(tape_product, *tape_model, paths, seed,
                                                         qm::first_aad_payoff, qm::RngKind::Philox);
        const double cpu_ms = ms_since(t0);

        double gap = 0.0, scale = 1e-300;
        for (std::size_t i = 0; i < cpu.risks.size(); ++i)
        {
            gap = std::max(gap, std::abs(cpu.risks[i] - gpu->risks[i]));
            scale = std::max(scale, std::abs(cpu.risks[i]));
        }
        std::printf("%-18s %-10s %7zu %11.0f %11.1f %9.0f %12.1e\n", name, model, cpu.risks.size(), cpu_ms, gpu_ms,
                    cpu_ms / gpu_ms, gap / scale);
    }
    std::printf("\nmax |gap|: largest difference between the two risk vectors, relative to the largest risk\n"
                "(the tape averages batches of 64 paths: exact only when the path count is a multiple of 64).\n");
    return 0;
}
