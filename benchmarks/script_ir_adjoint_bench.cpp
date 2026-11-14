// The CPU's risks of a script: Savine's tape against the adjoint built from
// the script's IR (blueprint/wp/27-ir-ad.md §7, lot L7). Same Philox paths,
// one thread; the largest gap between the two risk vectors is printed.
//
//   build/qm_script_ir_adjoint_bench [paths = 20000]

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

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
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

    std::vector<qm::Real> strikes()
    {
        std::vector<qm::Real> k;
        for (int i = 0; i < 30; ++i)
            k.push_back(40.0 + 5.0 * i);
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
    std::unique_ptr<qm::ISimulationModel<T>> make(const std::string &model)
    {
        if (model == "local_vol")
        {
            std::vector<T> g;
            for (qm::Real k : strikes())
                for (qm::Real t : maturities())
                    g.push_back(T(0.2 + 0.25 * (100.0 - k) / 100.0 + 0.01 * t));
            return std::make_unique<qm::LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.01), strikes(), maturities(), g,
                                                             1.0 / 252.0);
        }
        if (model == "heston")
            return std::make_unique<qm::BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05),
                                                          T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0), 1.0 / 252.0);
        return std::make_unique<qm::BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25));
    }

    double ms_since(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
} // namespace

int main(int argc, char **argv)
{
    const std::size_t paths = argc > 1 ? static_cast<std::size_t>(std::atol(argv[1])) : 20000;
    const qm::ValuationContext ctx{qm::Date::from_iso("2026-06-01")};
    const std::uint64_t seed = 7;
    std::printf("%zu paths, Philox, one CPU thread: the tape against the adjoint from the IR\n\n", paths);
    std::printf("%-18s %-14s %6s %10s %10s %8s %10s\n", "script", "model", "risks", "tape ms", "IR ms", "ratio",
                "max |gap|");
    const std::pair<const char *, const char *> cases[] = {
        {"european-call", "black_scholes"}, {"asian-call", "black_scholes"}, {"phoenix-autocall", "black_scholes"}, {"up-and-out-call", "heston"}, {"variance-swap", "heston"}, {"european-call", "local_vol"}, {"up-and-out-call", "local_vol"}, {"phoenix-autocall", "local_vol"}};
    for (const auto &[name, model] : cases)
    {
        const std::string src = read_script(name);
        if (src.empty())
            continue;
        const qm::ScriptedProduct<qm::Real> product(src, ctx);
        auto ir_model = make<qm::Real>(model);
        std::string why;
        auto t0 = std::chrono::steady_clock::now();
        const auto ir = qm::simulate_script_aad_ir(product, *ir_model, paths, seed, why);
        const double ir_ms = ms_since(t0);
        if (!ir)
        {
            std::printf("%-18s %-14s  (IR: %s)\n", name, model, why.c_str());
            continue;
        }
        const qm::ScriptedProduct<qm::aad::Number> tape_product(src, ctx);
        auto tape_model = make<qm::aad::Number>(model);
        t0 = std::chrono::steady_clock::now();
        const qm::AADSimulResults tape = qm::simulate_aad(tape_product, *tape_model, paths, seed,
                                                          qm::first_aad_payoff, qm::RngKind::Philox);
        const double tape_ms = ms_since(t0);
        double gap = 0.0, scale = 1e-300;
        for (std::size_t i = 0; i < tape.risks.size(); ++i)
        {
            gap = std::max(gap, std::abs(tape.risks[i] - ir->risks[i]));
            scale = std::max(scale, std::abs(tape.risks[i]));
        }
        std::printf("%-18s %-14s %6zu %10.0f %10.0f %7.1fx %10.1e\n", name, model, tape.risks.size(), tape_ms, ir_ms,
                    tape_ms / ir_ms, gap / scale);
    }
    return 0;
}
