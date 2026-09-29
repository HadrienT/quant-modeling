#include <gtest/gtest.h>

#include "quantModeling/models/credit/merton_structural.hpp"

#include <cmath>

namespace quantModeling
{

    // Equity + debt = assets: the balance sheet identity is the model's
    // put-call parity.
    TEST(MertonStructural, EquityPlusDebtIsTheFirm)
    {
        const MertonFirm f{100.0, 0.25, 70.0, 0.04};
        for (const Time T : {0.5, 1.0, 5.0})
            EXPECT_NEAR(f.equity_value(T) + f.debt_value(T), 100.0, 1e-12);
    }

    // Risky debt = riskless debt minus expected loss:
    //   B = D e^{-rT} (1 - PD (1 - R)),  so  s = -ln(1 - PD (1 - R)) / T.
    TEST(MertonStructural, SpreadIsDefaultProbabilityTimesLossGivenDefault)
    {
        const MertonFirm f{100.0, 0.35, 80.0, 0.03};
        for (const Time T : {1.0, 3.0, 10.0})
        {
            const Real pd = f.default_probability(T);
            const Real lgd = 1.0 - f.expected_recovery(T);
            EXPECT_NEAR(f.credit_spread(T), -std::log(1.0 - pd * lgd) / T, 1e-12) << T;
            EXPECT_GT(lgd, 0.0);
            EXPECT_LT(lgd, 1.0);
        }
    }

    TEST(MertonStructural, SpreadRisesWithLeverageAndVolatility)
    {
        const Real T = 5.0;
        const Real base = MertonFirm{100.0, 0.25, 60.0, 0.04}.credit_spread(T);
        EXPECT_GT((MertonFirm{100.0, 0.25, 80.0, 0.04}.credit_spread(T)), base);
        EXPECT_GT((MertonFirm{100.0, 0.40, 60.0, 0.04}.credit_spread(T)), base);
        EXPECT_GT(base, 0.0);
    }

    // The model's known weakness, pinned so nobody "fixes" it by accident:
    // a healthy firm's spread vanishes at short maturities.
    TEST(MertonStructural, ShortDatedSpreadOfAHealthyFirmVanishes)
    {
        const MertonFirm f{100.0, 0.2, 50.0, 0.04};
        EXPECT_LT(f.credit_spread(0.25), 1e-10);
        EXPECT_GT(f.credit_spread(10.0), 1e-5);
    }

    TEST(MertonStructural, DistanceToDefaultMatchesDefinition)
    {
        const MertonFirm f{120.0, 0.3, 90.0, 0.05};
        const Real T = 1.0;
        const Real dd = (std::log(120.0 / 90.0) + (0.05 - 0.045) * T) / 0.3;
        EXPECT_NEAR(f.distance_to_default(T), dd, 1e-14);
        EXPECT_NEAR(f.default_probability(T), 0.5 * std::erfc(dd / std::sqrt(2.0)), 1e-15);
    }

    // Calibration inverts the model: from the equity value and equity vol a
    // known firm produces, recover its assets and asset vol.
    TEST(MertonCalibration, RecoversAKnownFirm)
    {
        for (const MertonFirm truth : {MertonFirm{100.0, 0.25, 70.0, 0.04},
                                       MertonFirm{50.0, 0.10, 45.0, 0.02},
                                       MertonFirm{300.0, 0.6, 100.0, 0.05}})
        {
            const Real E = truth.equity_value(1.0);
            const Real sE = truth.equity_vol(1.0);
            const MertonCalibration c = calibrate_merton(E, sE, truth.debt_face, truth.rate, 1.0);
            EXPECT_TRUE(c.converged);
            EXPECT_NEAR(c.firm.asset_value, truth.asset_value, 1e-8 * truth.asset_value);
            EXPECT_NEAR(c.firm.asset_vol, truth.asset_vol, 1e-9);
            EXPECT_LT(c.relative_residual, 1e-10);
        }
    }

    // Equity is levered assets: σ_E > σ_V, the more so the higher the debt.
    TEST(MertonCalibration, AssetVolIsBelowEquityVol)
    {
        const MertonCalibration low = calibrate_merton(100.0, 0.3, 20.0, 0.04);
        const MertonCalibration high = calibrate_merton(100.0, 0.3, 200.0, 0.04);
        EXPECT_LT(low.firm.asset_vol, 0.3);
        EXPECT_LT(high.firm.asset_vol, low.firm.asset_vol);
    }

    TEST(MertonCalibration, RejectsNonPositiveInputs)
    {
        EXPECT_THROW(calibrate_merton(0.0, 0.3, 10.0, 0.04), InvalidInput);
        EXPECT_THROW(calibrate_merton(10.0, 0.3, 10.0, 0.04, 0.0), InvalidInput);
    }

} // namespace quantModeling
