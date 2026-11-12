#include <gtest/gtest.h>

#include <string>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/utils/sobol.hpp"

// Lot P1c of blueprint/wp/26-performance.md: Sobol RQMC is the default
// sampler everywhere (decided 2026-11-10). What runs Sobol says so; what
// cannot -- a hand-written engine without a Sobol path, a path beyond the
// generator's 21 201 dimensions -- runs pseudo-random and says why.

namespace quantModeling
{
    namespace
    {
        bool says(const std::string &diagnostics, const std::string &what)
        {
            return diagnostics.find(what) != std::string::npos;
        }
    } // namespace

    // A Sobol net is balanced at 2^m points: per replicate, the power of two
    // nearest to paths / replicates on a log scale (WP 26 P1c, #214).
    TEST(SamplerDefault, SobolReplicatesHoldAPowerOfTwoPoints)
    {
        EXPECT_EQ(sobol_points_per_replicate(200000, 16), 16384u);  // 12 500 -> 2^14 (12 500 > 8 192 sqrt 2)
        EXPECT_EQ(sobol_points_per_replicate(100000, 16), 8192u);   // 6 250 -> 2^13 (6 250 > 4 096 sqrt 2 = 5 793)
        EXPECT_EQ(sobol_points_per_replicate(1 << 20, 16), 65536u); // already a power of two
        EXPECT_EQ(sobol_points_per_replicate(5, 16), 1u);           // at least one point
        for (std::size_t paths = 16; paths < 2000000; paths = paths * 3 / 2 + 1)
        {
            const std::size_t n = sobol_points_per_replicate(paths, 16);
            EXPECT_EQ(n & (n - 1), 0u) << paths; // a power of two
            const double ratio = static_cast<double>(n * 16) / static_cast<double>(std::max<std::size_t>(16, paths));
            EXPECT_GT(ratio, 0.70) << paths; // within [1/sqrt 2, sqrt 2] of the request
            EXPECT_LT(ratio, 1.42) << paths;
        }
    }

    TEST(SamplerDefault, SobolIsTheDefault)
    {
        EXPECT_EQ(PricingSettings{}.mc_sampler, SamplerKind::Sobol);
    }

    // The hand-written BS vanilla has a Sobol path: it takes the default.
    TEST(SamplerDefault, TheVanillaEngineRunsSobol)
    {
        VanillaBSInput in{100.0, 100.0, 1.0, 0.03, 0.0, 0.2, true};
        in.n_paths = 20000;
        const PricingResult r = default_registry().price(
            {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::MonteCarlo, PricingInput{in}});
        EXPECT_TRUE(says(r.diagnostics, "Sobol")) << r.diagnostics;
    }

    // A hand-written engine not yet a script (WP 26 P3) has none:
    // pseudo-random, and it says so.
    TEST(SamplerDefault, AnEngineWithoutSobolSaysItRunsPseudoRandom)
    {
        VarianceSwapBSInput in{};
        in.spot = 100.0;
        in.rate = 0.03;
        in.vol = 0.2;
        in.maturity = 1.0;
        in.strike_var = 0.04;
        in.n_paths = 20000;
        const PricingResult r = default_registry().price(
            {InstrumentKind::VarianceSwap, ModelKind::BlackScholes, EngineKind::MonteCarlo, PricingInput{in}});
        EXPECT_TRUE(says(r.diagnostics, "pseudo-random: this engine has no Sobol path yet")) << r.diagnostics;
    }

    // The barrier became a script (WP 26 P3): Sobol.
    TEST(SamplerDefault, TheBarrierScriptRunsSobol)
    {
        BarrierBSInput in{};
        in.spot = 100.0;
        in.strike = 100.0;
        in.maturity = 1.0;
        in.rate = 0.03;
        in.vol = 0.2;
        in.is_call = true;
        in.barrier_type = BarrierType::UpAndOut;
        in.barrier_level = 130.0;
        in.n_paths = 20000;
        const PricingResult r = default_registry().price(
            {InstrumentKind::EquityBarrierOption, ModelKind::BlackScholes, EngineKind::MonteCarlo, PricingInput{in}});
        EXPECT_TRUE(says(r.diagnostics, "Sobol")) << r.diagnostics;
    }

    // A path of more draws than Sobol's dimensions: pseudo-random, said.
    TEST(SamplerDefault, BeyondSobolsDimensionsThePathRunsPseudoRandom)
    {
        // Three assets, daily for 30 years: about 3 x 7 800 > 21 201 draws.
        const std::string script = "schedule(2026-06-02, 2056-06-01, 1D, TARGET, F)\n"
                                   "    acc = acc + spot(0) + spot(1) + spot(2)\n\n"
                                   "2056-06-02\n    pays acc\n";
        const ScriptedProduct<Real> product(script, ValuationContext{Date::from_iso("2026-06-01")});
        scripting::ScriptModelSpec spec;
        spec.rate = 0.03;
        spec.spots = {100.0, 100.0, 100.0};
        spec.dividends = {0.0, 0.0, 0.0};
        spec.vols = {0.2, 0.2, 0.2};
        spec.correlation = {1.0, 0.3, 0.3, 0.3, 1.0, 0.3, 0.3, 0.3, 1.0};
        auto model = scripting::make_script_model<Real>(spec);
        PricingSettings s;
        s.mc_paths = 64;
        const SimulationMCResult r = simulate_script(product, *model, s);
        EXPECT_TRUE(says(r.diagnostics, "pseudo-random:")) << r.diagnostics;
        EXPECT_TRUE(says(r.diagnostics, "Sobol dimensions")) << r.diagnostics;
    }

} // namespace quantModeling
