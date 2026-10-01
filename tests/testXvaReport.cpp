#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/risk/xva.hpp"
#include "quantModeling/risk/xva_report.hpp"

#include <cmath>
#include <vector>

// Lot X3 of blueprint/wp/23-xva.md: CVA and DVA of a netting set from the
// simulated cube, with their Monte-Carlo errors and the share of each trade.

namespace quantModeling
{
    namespace
    {
        constexpr Time kTenDays = 10.0 / 250.0;

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.03));
        }

        InterestRateSwap swap(const HullWhiteCurveModel &m, Time tenor, bool payer, Real notional)
        {
            const MultiCurve curves{m.discount(), m.projection()};
            const Real par = value_swap(make_swap(0.0, tenor, 0.0), curves).par_rate;
            return make_swap(0.0, tenor, par, 1, 4, notional, payer);
        }

        /// A payer 10Y of 100, a receiver 10Y of 60 that partly hedges it,
        /// and a payer 5Y of 50. `side` = -1 is the same book seen from the
        /// counterparty.
        ExposurePaths book(const HullWhiteCurveModel &m, std::size_t paths = 5000, Real side = 1.0,
                           std::uint64_t seed = 42, Time mpor = 0.0)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(swap(m, 10.0, true, 100.0), side);
            engine.add(swap(m, 10.0, false, 60.0), side);
            engine.add(swap(m, 5.0, true, 50.0), side);
            ExposureSimulationSettings settings;
            settings.paths = paths;
            settings.seed = seed;
            settings.grid.margin_period_of_risk = mpor;
            return engine.simulate(settings);
        }

        XvaInputs inputs()
        {
            XvaInputs in;
            in.counterparty = CreditCurve({2.0, 10.0}, {0.02, 0.035}); // a BBB-like curve
            in.own = CreditCurve(0.012);
            in.lgd_counterparty = 0.6;
            in.lgd_own = 0.55;
            return in;
        }
    } // namespace

    TEST(XvaReport, CvaAndDvaAreTheIntegralsOfTheExposureProfile)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m);
        const XvaInputs in = inputs();
        const XvaReport r = xva_report(paths, in);

        // The path-by-path estimate is the integral of the mean profile.
        const ExposureProfile profile = r.exposure.profile();
        EXPECT_NEAR(r.cva.value, cva_bilateral(profile, in.counterparty, in.own, in.lgd_counterparty), 1e-12);
        EXPECT_NEAR(r.dva.value, dva(profile, in.counterparty, in.own, in.lgd_own), 1e-12);
        EXPECT_LT(r.cva.value, 0.0);
        EXPECT_GT(r.dva.value, 0.0);
        EXPECT_GT(r.cva.error, 0.0);
        EXPECT_LT(r.cva.error, 0.1 * std::abs(r.cva.value));
        // First to default: the bank may default first, which removes scenarios.
        EXPECT_LT(r.cva_unilateral, r.cva.value);
    }

    TEST(XvaReport, CvaOfOneSideIsDvaOfTheOther)
    {
        // Same book, same paths, seen from each side (blueprint §7.4).
        const HullWhiteCurveModel m = model();
        const XvaInputs bank = inputs();
        XvaInputs counterparty = bank;
        std::swap(counterparty.counterparty, counterparty.own);
        std::swap(counterparty.lgd_counterparty, counterparty.lgd_own);

        const XvaReport b = xva_report(book(m), bank);
        const XvaReport c = xva_report(book(m, 5000, -1.0), counterparty);
        EXPECT_NEAR(b.cva.value, -c.dva.value, 1e-12);
        EXPECT_NEAR(b.dva.value, -c.cva.value, 1e-12);
        EXPECT_NEAR(b.cva.error, c.dva.error, 1e-12);
    }

    TEST(XvaReport, NoDefaultNoAdjustment)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 1000);
        XvaInputs in = inputs();
        in.counterparty = CreditCurve(0.0);
        const XvaReport no_counterparty_risk = xva_report(paths, in);
        EXPECT_DOUBLE_EQ(no_counterparty_risk.cva.value, 0.0);
        EXPECT_DOUBLE_EQ(no_counterparty_risk.cva_rule_of_thumb, 0.0);
        EXPECT_GT(no_counterparty_risk.dva.value, 0.0);

        in = inputs();
        in.own = CreditCurve(0.0);
        const XvaReport default_free_bank = xva_report(paths, in);
        EXPECT_DOUBLE_EQ(default_free_bank.dva.value, 0.0);
        EXPECT_NEAR(default_free_bank.cva.value, default_free_bank.cva_unilateral, 1e-13);
        // Linear in the loss given default.
        XvaInputs half = in;
        half.lgd_counterparty = 0.3;
        EXPECT_NEAR(xva_report(paths, half).cva.value, 0.5 * default_free_bank.cva.value, 1e-13);
    }

    TEST(XvaReport, MonteCarloErrorIsTheDispersionBetweenSeeds)
    {
        const HullWhiteCurveModel m = model();
        const XvaInputs in = inputs();
        const XvaReport a = xva_report(book(m, 4000, 1.0, 1), in);
        const XvaReport b = xva_report(book(m, 4000, 1.0, 2), in);
        EXPECT_NE(a.cva.value, b.cva.value);
        EXPECT_NEAR(a.cva.value, b.cva.value, 4.0 * std::hypot(a.cva.error, b.cva.error));
        EXPECT_NEAR(a.dva.value, b.dva.value, 4.0 * std::hypot(a.dva.error, b.dva.error));
        // Four times the paths, half the error.
        const XvaReport large = xva_report(book(m, 16000, 1.0, 1), in);
        EXPECT_NEAR(a.cva.error / large.cva.error, 2.0, 0.15);
    }

    TEST(XvaReport, SharesOfEachTrade)
    {
        const HullWhiteCurveModel m = model();
        const XvaReport r = xva_report(book(m), inputs());
        ASSERT_EQ(r.contributions.size(), 3u);

        // Marginal (Euler) shares add up to the CVA of the netting set.
        Real marginal = 0.0, standalone = 0.0;
        for (const TradeContribution &c : r.contributions)
        {
            marginal += c.marginal_cva;
            standalone += c.standalone_cva;
            EXPECT_LT(c.standalone_cva, 0.0);
        }
        EXPECT_NEAR(marginal, r.cva.value, 1e-12);
        // Netting: the set costs less than its trades taken one by one.
        EXPECT_GT(r.cva.value, standalone);

        // The 10Y payer and the 10Y receiver hedge each other: whichever is
        // added last reduces the CVA of what is already there, so both have
        // a positive incremental CVA although each has a cost on its own.
        // CVA({payer, receiver, 5Y}) > CVA({receiver, 5Y}) and > CVA({payer, 5Y}).
        EXPECT_GT(r.contributions[0].incremental_cva, 0.0);
        EXPECT_GT(r.contributions[1].incremental_cva, 0.0);
        // The 5Y payer adds to the net payer position: it costs.
        EXPECT_LT(r.contributions[2].incremental_cva, 0.0);
        // Marginal shares need not have the sign of the incremental ones, but
        // the hedge that is smaller than what it hedges has a positive share.
        EXPECT_GT(r.contributions[1].marginal_cva, 0.0);
        EXPECT_LT(r.contributions[0].marginal_cva, 0.0);
        // A trade's incremental cost never exceeds its stand-alone cost.
        for (const TradeContribution &c : r.contributions)
            EXPECT_GE(c.incremental_cva, c.standalone_cva - 1e-12);
    }

    TEST(XvaReport, SubsetOfTradesAndSingleTrade)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 2000);
        XvaInputs in = inputs();
        in.trades = {0};
        const XvaReport alone = xva_report(paths, in);
        ASSERT_EQ(alone.contributions.size(), 1u);
        // Alone, the three notions coincide.
        EXPECT_NEAR(alone.contributions[0].standalone_cva, alone.cva.value, 1e-12);
        EXPECT_NEAR(alone.contributions[0].incremental_cva, alone.cva.value, 1e-12);
        EXPECT_NEAR(alone.contributions[0].marginal_cva, alone.cva.value, 1e-12);
        // And it is the stand-alone figure reported inside the full set.
        in.trades = {};
        EXPECT_NEAR(xva_report(paths, in).contributions[0].standalone_cva, alone.cva.value, 1e-12);
    }

    TEST(XvaReport, RuleOfThumbGivesTheOrderOfMagnitude)
    {
        // CVA ≈ -spread × EPE × T (blueprint §7.2), unilateral, one swap.
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(swap(m, 5.0, true, 100.0));
        ExposureSimulationSettings settings;
        settings.paths = 10000;
        XvaInputs in;
        in.counterparty = CreditCurve(0.02 / 0.6); // 200 bp at 40 % recovery
        const XvaReport r = xva_report(engine.simulate(settings), in);
        EXPECT_NEAR(r.cva_rule_of_thumb / r.cva.value, 1.0, 0.12);
        EXPECT_NEAR(r.cva.value, r.cva_unilateral, 1e-13); // default-free bank
    }

    TEST(XvaReport, CollateralShrinksTheAdjustments)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 5000, 1.0, 42, kTenDays);
        XvaInputs in = inputs();
        const XvaReport open = xva_report(paths, in);

        Csa csa;
        csa.margin_period_of_risk = kTenDays;
        in.csa = csa;
        const XvaReport collateralised = xva_report(paths, in);
        EXPECT_LT(std::abs(collateralised.cva.value), 0.5 * std::abs(open.cva.value));
        EXPECT_LT(collateralised.dva.value, 0.5 * open.dva.value);
        EXPECT_LT(collateralised.cva.value, 0.0); // small, not zero: the margin period of risk
        // No exact allocation under a CSA; the incremental CVA still exists.
        for (const TradeContribution &c : collateralised.contributions)
        {
            EXPECT_TRUE(std::isnan(c.marginal_cva));
            EXPECT_TRUE(std::isfinite(c.incremental_cva));
        }
        // A threshold lets exposure build up again.
        in.csa->threshold_counterparty = 2.0;
        EXPECT_LT(xva_report(paths, in).cva.value, collateralised.cva.value);
    }

    TEST(XvaReport, FundingAdjustments)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 2000);
        XvaInputs in = inputs();
        EXPECT_DOUBLE_EQ(xva_report(paths, in).fca, 0.0);
        in.borrowing_spread = 0.008;
        in.lending_spread = 0.003;
        const XvaReport r = xva_report(paths, in);
        EXPECT_LT(r.fca, 0.0);
        EXPECT_GT(r.fba, 0.0);
    }

    TEST(XvaReport, RefusesHistoricalScenariosAndBadInputs)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(swap(m, 5.0, true, 100.0));
        ExposureSimulationSettings settings;
        settings.paths = 200;
        settings.historical = HistoricalRateDynamics{0.2, 0.03, 0.008};
        EXPECT_THROW(xva_report(engine.simulate(settings), inputs()), InvalidInput);

        const ExposurePaths paths = book(m, 200);
        XvaInputs in = inputs();
        in.lgd_counterparty = 1.4;
        EXPECT_THROW(xva_report(paths, in), InvalidInput);
        in = inputs();
        in.trades = {7};
        EXPECT_THROW(xva_report(paths, in), InvalidInput);
        // A CSA needs the lagged dates on the grid.
        in = inputs();
        in.csa = Csa{};
        EXPECT_THROW(xva_report(paths, in), InvalidInput);
    }

} // namespace quantModeling
