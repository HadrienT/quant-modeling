#include <gtest/gtest.h>

#include "quantModeling/models/equity/sabr_pde.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr Real kForward = 100.0;

        std::vector<Real> strike_grid(Real lo, Real hi, int n)
        {
            std::vector<Real> ks(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                ks[static_cast<std::size_t>(i)] = lo + (hi - lo) * static_cast<Real>(i) / static_cast<Real>(n - 1);
            return ks;
        }
    } // namespace

    // ── Exact reference: beta=1, nu=0 is pure lognormal -- the PDE must ────
    // reproduce Black-76 at vol=alpha for every strike, not just fit a curve
    // that looks plausible.

    TEST(SABRPDE, MatchesBlackScholesExactlyInTheDegenerateLognormalCase)
    {
        constexpr Real alpha = 0.25, ttm = 1.0;
        const SABRParams p{alpha, /*beta=*/1.0, /*rho=*/0.0, /*nu=*/0.0};
        const std::vector<Real> strikes = strike_grid(70.0, 130.0, 13);

        const SABRPDEResult result = sabr_arbitrage_free_prices(kForward, ttm, p, strikes);

        for (std::size_t i = 0; i < strikes.size(); ++i)
        {
            const Real reference = black76_call_price(kForward, strikes[i], ttm, alpha, 1.0);
            EXPECT_NEAR(result.call_prices[i], reference, 0.02) << "strike=" << strikes[i];
        }
    }

    TEST(SABRPDE, MatchesBlackScholesAcrossSeveralMaturitiesInTheDegenerateCase)
    {
        const SABRParams p{0.35, 1.0, 0.0, 0.0};
        const std::vector<Real> strikes = {80.0, 100.0, 120.0};
        for (const Real ttm : {0.1, 0.5, 2.0})
        {
            const SABRPDEResult result = sabr_arbitrage_free_prices(kForward, ttm, p, strikes);
            for (std::size_t i = 0; i < strikes.size(); ++i)
            {
                const Real reference = black76_call_price(kForward, strikes[i], ttm, 0.35, 1.0);
                EXPECT_NEAR(result.call_prices[i], reference, 0.03) << "ttm=" << ttm << " strike=" << strikes[i];
            }
        }
    }

    // ── Arbitrage-free by construction, checked where the closed form fails ──

    TEST(SABRPDE, CallPricesAreConvexInStrikeWhereTheClosedFormHasNegativeDensity)
    {
        // Low beta, high nu, long maturity: the textbook regime where
        // sabr_implied_vol's asymptotic expansion produces a negative
        // density at low strikes.
        const SABRParams p{0.40, 0.20, -0.60, 1.20};
        constexpr Real ttm = 3.0;
        const std::vector<Real> strikes = strike_grid(5.0, 200.0, 60);

        const SABRPDEResult result = sabr_arbitrage_free_prices(kForward, ttm, p, strikes);

        for (std::size_t i = 1; i + 1 < strikes.size(); ++i)
        {
            const Real c0 = result.call_prices[i - 1];
            const Real c1 = result.call_prices[i];
            const Real c2 = result.call_prices[i + 1];
            EXPECT_GE(c0 - 2.0 * c1 + c2, -1e-6) << "strike=" << strikes[i];
        }
    }

    TEST(SABRPDE, CallPricesAreMonotonicallyDecreasingInStrike)
    {
        const SABRParams p{0.40, 0.20, -0.60, 1.20};
        const std::vector<Real> strikes = strike_grid(5.0, 200.0, 40);
        const SABRPDEResult result = sabr_arbitrage_free_prices(kForward, 2.0, p, strikes);

        for (std::size_t i = 1; i < strikes.size(); ++i)
            EXPECT_LE(result.call_prices[i], result.call_prices[i - 1] + 1e-9);
    }

    TEST(SABRPDE, CallPricesStayWithinNoArbitrageBounds)
    {
        const SABRParams p{0.30, 0.5, 0.2, 0.8};
        const std::vector<Real> strikes = strike_grid(10.0, 250.0, 30);
        const SABRPDEResult result = sabr_arbitrage_free_prices(kForward, 1.5, p, strikes);

        for (std::size_t i = 0; i < strikes.size(); ++i)
        {
            EXPECT_GE(result.call_prices[i], -1e-9);
            EXPECT_LE(result.call_prices[i], kForward + 1e-9);
        }
    }

    // ── Agreement with the closed form where the closed form is trustworthy ──

    TEST(SABRPDE, AgreesWithTheClassicFormulaInAWellBehavedRegime)
    {
        // Moderate parameters, short maturity: sabr_implied_vol should be
        // reliable here, so the PDE (which fixes a problem that is not
        // present in this regime) should land close to it.
        const SABRParams p{0.22, 0.7, -0.25, 0.35};
        constexpr Real ttm = 0.5;
        const Real df = 1.0;
        const std::vector<Real> strikes = {85.0, 95.0, 100.0, 105.0, 115.0};

        const std::vector<Real> pde_ivs = sabr_arbitrage_free_implied_vols(kForward, ttm, p, strikes, df);

        for (std::size_t i = 0; i < strikes.size(); ++i)
        {
            const Real classic = sabr_implied_vol(kForward, strikes[i], ttm, p);
            ASSERT_FALSE(std::isnan(pde_ivs[i])) << "strike=" << strikes[i];
            EXPECT_NEAR(pde_ivs[i], classic, 0.01) << "strike=" << strikes[i];
        }
    }

    // ── Grid refinement changes the answer by less, not more ────────────────

    TEST(SABRPDE, RefiningTheGridChangesThePriceLessThanACoarseGridDoes)
    {
        const SABRParams p{0.30, 0.5, -0.3, 0.7};
        const std::vector<Real> strikes = {100.0};

        SABRPDESettings coarse;
        coarse.n_space = 40;
        coarse.n_time = 20;
        SABRPDESettings medium;
        medium.n_space = 80;
        medium.n_time = 40;
        SABRPDESettings fine;
        fine.n_space = 160;
        fine.n_time = 80;

        const Real p_coarse = sabr_arbitrage_free_prices(kForward, 1.0, p, strikes, coarse).call_prices[0];
        const Real p_medium = sabr_arbitrage_free_prices(kForward, 1.0, p, strikes, medium).call_prices[0];
        const Real p_fine = sabr_arbitrage_free_prices(kForward, 1.0, p, strikes, fine).call_prices[0];

        EXPECT_LT(std::fabs(p_fine - p_medium), std::fabs(p_medium - p_coarse));
    }

} // namespace quantModeling
