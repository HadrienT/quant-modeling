#include <benchmark/benchmark.h>

#include <memory>

#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/gaussian_source.hpp"

namespace qm = quantModeling;

namespace
{
    constexpr qm::Real S0 = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.20;

    qm::PricingContext make_ctx(int n_paths, bool antithetic, qm::GaussianKind kind)
    {
        qm::PricingContext ctx;
        ctx.model = std::make_shared<qm::BlackScholesModel>(S0, r, q, sigma);
        ctx.settings.mc_paths = n_paths;
        ctx.settings.mc_seed = 42;
        ctx.settings.mc_antithetic = antithetic;
        ctx.settings.mc_gaussian = kind;
        return ctx;
    }

    // Full engine benchmark: visitor + kernel + result assembly.
    void bench_engine(benchmark::State &state, bool antithetic, qm::GaussianKind kind)
    {
        const int n_paths = static_cast<int>(state.range(0));
        const auto ctx = make_ctx(n_paths, antithetic, kind);
        const qm::VanillaOption opt(
            std::make_shared<qm::PlainVanillaPayoff>(qm::OptionType::Call, K),
            std::make_shared<qm::EuropeanExercise>(T));

        for (auto _ : state)
        {
            qm::BSEuroVanillaMCEngine engine(ctx);
            opt.accept(engine);
            double npv = engine.results().npv;
            benchmark::DoNotOptimize(npv);
        }
        state.SetItemsProcessed(state.iterations() * n_paths);
    }
} // namespace

static void BM_VanillaMC_BoxMuller(benchmark::State &state)
{
    bench_engine(state, false, qm::GaussianKind::BoxMuller);
}
static void BM_VanillaMC_BoxMuller_Antithetic(benchmark::State &state)
{
    bench_engine(state, true, qm::GaussianKind::BoxMuller);
}
static void BM_VanillaMC_InverseNormal(benchmark::State &state)
{
    bench_engine(state, false, qm::GaussianKind::InverseNormal);
}
static void BM_VanillaMC_InverseNormal_Antithetic(benchmark::State &state)
{
    bench_engine(state, true, qm::GaussianKind::InverseNormal);
}

BENCHMARK(BM_VanillaMC_BoxMuller)->Arg(1 << 20);
BENCHMARK(BM_VanillaMC_BoxMuller_Antithetic)->Arg(1 << 20);
BENCHMARK(BM_VanillaMC_InverseNormal)->Arg(1 << 20);
BENCHMARK(BM_VanillaMC_InverseNormal_Antithetic)->Arg(1 << 20);
