#include <gtest/gtest.h>

#include "quantModeling/market/sabr_calibration.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        /// A synthetic, noiseless smile generated from known SABR parameters:
        /// every quote's implied vol agrees exactly with `truth`, so
        /// calibration should recover the smile functionally.
        std::vector<SABRSliceQuote> synthetic_quotes(Real forward, Real ttm, const SABRParams &truth)
        {
            std::vector<SABRSliceQuote> quotes;
            for (Real strike = 60.0; strike <= 150.0; strike += 5.0)
            {
                SABRSliceQuote q;
                q.strike = strike;
                q.market_iv = sabr_implied_vol(forward, strike, ttm, truth);
                q.weight = 1.0;
                quotes.push_back(q);
            }
            return quotes;
        }

    } // namespace

    TEST(SABRCalibration, EmptyQuotesDoNotCrashAndReturnDefaultParams)
    {
        const auto result = calibrate_sabr_slice({}, /*forward=*/100.0, /*ttm=*/0.5, /*beta=*/0.5);
        EXPECT_EQ(result.report.params.size(), 0u);
        EXPECT_DOUBLE_EQ(result.params.beta, 0.5); // beta is threaded through even on the empty-input fallback
    }

    TEST(SABRCalibration, RecoversTheSmileFunctionallyFromNoiselessSyntheticQuotes)
    {
        const Real forward = 100.0, ttm = 0.75, beta = 0.5;
        const SABRParams truth{/*alpha=*/0.25, beta, /*rho=*/-0.35, /*nu=*/0.6};

        const auto quotes = synthetic_quotes(forward, ttm, truth);
        const auto result = calibrate_sabr_slice(quotes, forward, ttm, beta);

        EXPECT_TRUE(result.report.converged);
        // alpha/rho/nu are not always individually identifiable to high
        // precision from a bounded strike range (SABR's parameters can trade
        // off against each other, same rationale as SVI's calibration
        // test), so the property that must hold is the *fit*, not exact
        // parameter equality.
        for (const auto &q : quotes)
        {
            const Real fitted_iv = sabr_implied_vol(forward, q.strike, ttm, result.params);
            EXPECT_NEAR(fitted_iv, q.market_iv, 2e-3);
        }
        EXPECT_LT(result.report.rmse, 2e-3);
        EXPECT_DOUBLE_EQ(result.params.beta, beta);
    }

    TEST(SABRCalibration, RecoversASmileWithADifferentFixedBeta)
    {
        // beta = 1 (lognormal-style) is a materially different regime from
        // the other test's beta = 0.5 -- checks the fixed-beta plumbing
        // itself, not just one convenient value.
        const Real forward = 100.0, ttm = 0.5, beta = 1.0;
        const SABRParams truth{/*alpha=*/0.20, beta, /*rho=*/0.2, /*nu=*/0.4};

        const auto quotes = synthetic_quotes(forward, ttm, truth);
        const auto result = calibrate_sabr_slice(quotes, forward, ttm, beta);

        EXPECT_TRUE(result.report.converged);
        EXPECT_LT(result.report.rmse, 2e-3);
        EXPECT_DOUBLE_EQ(result.params.beta, beta);
    }

    TEST(SABRCalibration, FittedParamsRespectTheObjectiveBounds)
    {
        const Real forward = 100.0, ttm = 0.5, beta = 0.5;
        const SABRParams truth{0.25, beta, -0.35, 0.6};
        const auto quotes = synthetic_quotes(forward, ttm, truth);

        const SABRSliceObjective objective(quotes, forward, ttm, beta);
        const auto lower = objective.lower_bounds();
        const auto upper = objective.upper_bounds();

        const auto result = calibrate_sabr_slice(quotes, forward, ttm, beta);
        EXPECT_GE(result.params.alpha, lower[0] - 1e-9);
        EXPECT_LE(result.params.alpha, upper[0] + 1e-9);
        EXPECT_GE(result.params.rho, lower[1] - 1e-9);
        EXPECT_LE(result.params.rho, upper[1] + 1e-9);
        EXPECT_GE(result.params.nu, lower[2] - 1e-9);
        EXPECT_LE(result.params.nu, upper[2] + 1e-9);
    }

    TEST(SABRCalibration, PackAndUnpackRoundTrip)
    {
        const SABRParams p{0.22, 0.5, -0.4, 0.7};
        const auto packed = SABRSliceObjective::pack(p);
        ASSERT_EQ(packed.size(), 3u);
        const SABRParams unpacked = SABRSliceObjective::unpack(packed, p.beta);
        EXPECT_DOUBLE_EQ(unpacked.alpha, p.alpha);
        EXPECT_DOUBLE_EQ(unpacked.beta, p.beta);
        EXPECT_DOUBLE_EQ(unpacked.rho, p.rho);
        EXPECT_DOUBLE_EQ(unpacked.nu, p.nu);
    }

} // namespace quantModeling
