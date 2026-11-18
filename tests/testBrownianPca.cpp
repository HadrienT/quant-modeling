#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/stats.hpp"

// The principal-component construction of the Brownian paths
// (brownian_pca_matrix, WP 26 #226): it is a square root of the Brownian
// covariance, its components come by decreasing variance, the GPU's flat
// path draws the generic engine's points under it, and it prices without
// bias.

namespace quantModeling
{
    namespace
    {
        const std::vector<Time> kTimes{0.05, 0.1, 0.2, 0.35, 0.5, 0.8, 1.0, 1.4, 2.0};
    }

    TEST(BrownianPca, IsASquareRootOfTheBrownianCovariance)
    {
        const std::size_t n = kTimes.size();
        const std::vector<Real> a = brownian_pca_matrix(kTimes);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
            {
                double c = 0.0;
                for (std::size_t k = 0; k < n; ++k)
                    c += a[i * n + k] * a[j * n + k];
                EXPECT_NEAR(c, std::min(kTimes[i], kTimes[j]), 1e-12) << i << " " << j;
            }
        // components by decreasing variance (the squared norm of a column)
        double prev = 1e300;
        for (std::size_t k = 0; k < n; ++k)
        {
            double v = 0.0;
            for (std::size_t s = 0; s < n; ++s)
                v += a[s * n + k] * a[s * n + k];
            EXPECT_LE(v, prev * (1 + 1e-12)) << k;
            prev = v;
        }
    }

    TEST(BrownianPca, FlatPointEqualsTheGenericEngines)
    {
        const std::size_t dim = kTimes.size() * 2;
        const BrownianLayout layout{kTimes, 2, 2};
        const mc::SobolTables t = mc::sobol_tables(dim, 42, 2, layout, true, false, false, true);
        ASSERT_FALSE(t.pca.empty());
        const mc::SobolBridgeView v = t.view();
        std::vector<double> pt(dim), out(dim), point(dim), ref(dim);
        for (int b = 0; b < 2; ++b)
        {
            SobolSequence seq(static_cast<int>(dim), (static_cast<uint64_t>(42u) << 32) | static_cast<uint64_t>(b));
            BridgedGaussians g(kTimes, 2, 2, /*pca=*/true);
            for (uint32_t p = 0; p < 300; ++p)
            {
                seq.next_gaussian(point);
                g.map(point, ref);
                mc::sobol_bridged_gaussians(v, static_cast<uint64_t>(b), p, pt.data(), 1, out.data(), 1);
                for (std::size_t j = 0; j < dim; ++j)
                    ASSERT_EQ(out[j], ref[j]) << b << " " << p << " " << j;
            }
        }
    }

    TEST(BrownianPca, PricesACallAndAnAsianWithoutBias)
    {
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const double S = 100, r = 0.03, q = 0.01, s = 0.25;
        // a call: the closed form; an Asian on 12 monthly fixings: the
        // bridge's price, within both errors
        const ScriptedProduct<Real> call("2027-06-01\n    pays max(spot() - 100, 0)\n", ctx);
        const ScriptedProduct<Real> asian("2026-06-02\n    acc = 0\n"
                                          "schedule(2026-07-01, 2027-06-01, 1M, NONE, F)\n    acc = acc + spot()\n"
                                          "2027-06-01\n    pays max(acc / 12 - 100, 0)\n",
                                          ctx);
        const double T = ctx.t(Date::from_iso("2027-06-01"));
        const double d1 = (std::log(S / 100) + (r - q + 0.5 * s * s) * T) / (s * std::sqrt(T));
        const double exact = S * std::exp(-q * T) * norm_cdf(d1) - 100 * std::exp(-r * T) * norm_cdf(d1 - s * std::sqrt(T));
        PricingSettings set;
        set.mc_paths = 1 << 16;
        set.mc_sampler = SamplerKind::Sobol;
        set.mc_brownian_pca = true;
        BlackScholesSimModel<Real> m1(S, r, q, s), m2(S, r, q, s), m3(S, r, q, s);
        const SimulationMCResult c = simulate<Real>(call, m1, set);
        EXPECT_NE(c.diagnostics.find("principal components"), std::string::npos) << c.diagnostics;
        EXPECT_NEAR(c.npv(), exact, 4.0 * c.std_error() + 1e-12);
        const SimulationMCResult a = simulate<Real>(asian, m2, set);
        set.mc_brownian_pca = false;
        const SimulationMCResult b = simulate<Real>(asian, m3, set);
        EXPECT_NEAR(a.npv(), b.npv(), 4.0 * std::hypot(a.std_error(), b.std_error()));
    }

} // namespace quantModeling
