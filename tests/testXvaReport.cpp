#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/risk/xva.hpp"
#include "quantModeling/risk/xva_report.hpp"

#include <algorithm>
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

    // ── Lot X5: margin and capital ───────────────────────────────────────────

    namespace
    {
        /// The three swaps of book(), as SA-CCR describes them.
        std::vector<sa_ccr::Trade> book_trades()
        {
            const auto trade = [](Time tenor, bool payer, Real notional)
            {
                sa_ccr::Trade t;
                t.subclass = sa_ccr::SubClass::InterestRate;
                t.hedging_set = "USD";
                t.notional = notional;
                t.long_primary_risk_factor = payer;
                t.maturity = tenor;
                t.end = tenor;
                return t;
            };
            return {trade(10.0, true, 100.0), trade(10.0, false, 60.0), trade(5.0, true, 50.0)};
        }

        XvaInputs margined_inputs()
        {
            XvaInputs in = inputs();
            Csa csa;
            csa.margin_period_of_risk = kTenDays;
            in.csa = csa;
            in.borrowing_spread = 0.006;
            in.lending_spread = 0.002;
            in.initial_margin_spread = 0.006;
            return in;
        }
    } // namespace

    TEST(XvaReport, InitialMarginTurnsTheCvaIntoAnMva)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 10000, 1.0, 42, kTenDays);
        const XvaInputs variation_only = margined_inputs();
        XvaInputs with_margin = variation_only;
        with_margin.initial_margin = DimSettings{};

        const XvaReport vm = xva_report(paths, variation_only);
        const XvaReport im = xva_report(paths, with_margin);
        EXPECT_EQ(vm.mva, 0.0);
        EXPECT_EQ(vm.initial_margin_today, 0.0);
        EXPECT_GT(im.initial_margin_today, 0.0);
        ASSERT_EQ(im.expected_initial_margin.size(), im.exposure.times.size());

        // The margin covers 99 % of the ten-day move: what is left of the
        // counterparty risk is a fraction of what variation margin left...
        EXPECT_LT(im.cva.value, 0.0);
        EXPECT_GT(im.cva.value, 0.05 * vm.cva.value);
        EXPECT_LT(im.dva.value, 0.05 * vm.dva.value);
        // ...and its price is the funding of the margin posted, which here
        // costs more than the CVA it removes.
        EXPECT_LT(im.mva, 0.0);
        EXPECT_GT(-im.mva, im.cva.value - vm.cva.value);
        // Segregated margin funds nothing: FCA and FBA are unchanged.
        EXPECT_DOUBLE_EQ(im.fca, vm.fca);
        EXPECT_DOUBLE_EQ(im.fba, vm.fba);
        // Each trade still has its figures, under its own margin.
        for (const TradeContribution &c : im.contributions)
        {
            EXPECT_TRUE(std::isfinite(c.standalone_cva));
            EXPECT_TRUE(std::isfinite(c.incremental_cva));
            EXPECT_LE(c.standalone_cva, 0.0);
        }

        // The margin earns what it costs to fund: no MVA.
        with_margin.initial_margin_spread = 0.0;
        EXPECT_EQ(xva_report(paths, with_margin).mva, 0.0);
        // Anchored on an amount computed elsewhere, twice the model's: twice
        // the MVA.
        with_margin.initial_margin_spread = 0.006;
        with_margin.initial_margin->im_today = 2.0 * im.initial_margin_today;
        EXPECT_NEAR(xva_report(paths, with_margin).mva, 2.0 * im.mva, 1e-9 * std::abs(im.mva));

        // Initial margin without variation margin is not a set-up.
        XvaInputs open = inputs();
        open.initial_margin = DimSettings{};
        EXPECT_THROW(xva_report(book(m, 500), open), InvalidInput);
    }

    TEST(XvaReport, ColvaAndKva)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 5000, 1.0, 42, kTenDays);
        XvaInputs in = margined_inputs();
        EXPECT_EQ(xva_report(paths, in).colva, 0.0); // the CSA pays the discount rate
        EXPECT_EQ(xva_report(paths, in).kva, 0.0);   // no capital inputs
        EXPECT_FALSE(xva_report(paths, in).capital.has_value());

        // A CSA paying 20 bp under the discount rate. The book is at par
        // today and its expected value drifts: whichever side holds the
        // collateral on average earns or loses those 20 bp.
        in.collateral_spread = -0.002;
        const Real gain = xva_report(paths, in).colva;
        in.collateral_spread = 0.002;
        EXPECT_DOUBLE_EQ(xva_report(paths, in).colva, -gain);
        EXPECT_NE(gain, 0.0);
        // No CSA, no collateral.
        XvaInputs open = inputs();
        open.collateral_spread = 0.002;
        EXPECT_EQ(xva_report(book(m, 500), open).colva, 0.0);

        CapitalInputs capital;
        capital.trades = book_trades();
        capital.pd = 0.02;
        capital.sector = ba_cva::Sector::Financial;
        capital.quality = ba_cva::CreditQuality::InvestmentGrade;
        open.capital = capital;
        const XvaReport uncollateralised = xva_report(book(m, 5000), open);
        ASSERT_TRUE(uncollateralised.capital.has_value());
        EXPECT_LT(uncollateralised.kva, 0.0);
        EXPECT_GT(uncollateralised.capital->ead_today, 0.0);

        // Under the CSA: the margined SA-CCR, a fraction of the capital.
        in.capital = capital;
        in.capital->margin = sa_ccr::MarginAgreement{0.0, 0.0, 0.0, kTenDays};
        const XvaReport margined = xva_report(paths, in);
        EXPECT_LT(margined.kva, 0.0);
        EXPECT_GT(margined.kva, 0.6 * uncollateralised.kva);
        // With initial margin on top: less again.
        in.initial_margin = DimSettings{};
        EXPECT_GT(xva_report(paths, in).kva, margined.kva);
        // A higher return required on capital costs proportionally more.
        open.cost_of_capital = 0.15;
        EXPECT_NEAR(xva_report(book(m, 5000), open).kva, 1.5 * uncollateralised.kva,
                    1e-12 * std::abs(uncollateralised.kva));

        // The capital inputs must describe the netted trades.
        open.capital->trades.pop_back();
        EXPECT_THROW(xva_report(book(m, 500), open), InvalidInput);
        open = inputs();
        open.cost_of_capital = -0.1;
        EXPECT_THROW(xva_report(book(m, 500), open), InvalidInput);
    }

    // ── Lot X9: every adjustment with its Monte-Carlo error ──────────────────

    namespace
    {
        struct Dispersion
        {
            std::vector<Real> values;
            Real reported = 0.0;
            void add(Real value, Real error)
            {
                values.push_back(value);
                reported += error;
            }
            /// Standard deviation between the runs over the mean reported error.
            Real ratio() const
            {
                const Real count = static_cast<Real>(values.size());
                Real mean = 0.0, variance = 0.0;
                for (const Real v : values)
                    mean += v / count;
                for (const Real v : values)
                    variance += (v - mean) * (v - mean) / (count - 1.0);
                return std::sqrt(variance) / (reported / count);
            }
        };
    } // namespace

    TEST(XvaReport, TheErrorOfEachAdjustmentIsItsDispersionBetweenSeeds)
    {
        const HullWhiteCurveModel m = model();
        // Uncollateralised: funding both ways, capital by both methods.
        XvaInputs open = inputs();
        open.borrowing_spread = 0.008;
        open.lending_spread = 0.003;
        CapitalInputs capital;
        capital.trades = book_trades();
        capital.pd = 0.02;
        open.capital = capital;
        XvaInputs internal = open;
        internal.capital->method = ExposureMethod::InternalModel;
        // Under a CSA that pays 20 bp over the discount rate, with initial
        // margin.
        XvaInputs margined = margined_inputs();
        margined.collateral_spread = 0.002;
        margined.initial_margin = DimSettings{};
        margined.initial_margin_spread = 0.006;

        Dispersion fca, fba, fva, kva, kva_internal, epe, eepe, pfe, colva, mva;
        const std::size_t runs = 24;
        std::size_t peak = 0;
        for (std::uint64_t seed = 1; seed <= runs; ++seed)
        {
            const ExposurePaths paths = book(m, 2000, 1.0, seed);
            const XvaReport r = xva_report(paths, open);
            fca.add(r.fca, r.fca_error);
            fba.add(r.fba, r.fba_error);
            fva.add(r.fca + r.fba, r.fva_error);
            kva.add(r.kva, r.kva_error);
            epe.add(r.exposure.epe, r.exposure.epe_error);
            eepe.add(r.exposure.eepe, r.exposure.eepe_error);
            if (seed == 1)
                peak = static_cast<std::size_t>(
                    std::max_element(r.exposure.pfe.begin(), r.exposure.pfe.end()) -
                    r.exposure.pfe.begin());
            pfe.add(r.exposure.pfe[peak], r.exposure.pfe_error[peak]);
            const XvaReport i = xva_report(paths, internal);
            kva_internal.add(i.kva, i.kva_error);

            const XvaReport c = xva_report(book(m, 2000, 1.0, seed, kTenDays), margined);
            colva.add(c.colva, c.colva_error);
            mva.add(c.mva, c.mva_error);
        }
        // Measured over forty runs: every ratio between 0.91 and 1.06, the
        // MVA's 1.13. Twenty-four give a standard deviation to about 15 %; the batch
        // errors (EPE, Effective EPE, KVA) are themselves estimates.
        for (const Dispersion *d : {&fca, &fba, &fva, &colva})
        {
            EXPECT_GT(d->ratio(), 0.7);
            EXPECT_LT(d->ratio(), 1.4);
        }
        for (const Dispersion *d : {&kva, &kva_internal, &epe, &eepe, &pfe})
        {
            EXPECT_GT(d->ratio(), 0.6);
            EXPECT_LT(d->ratio(), 1.6);
        }
        // The MVA's error is conditional on the fitted margin model: between
        // seeds the regression moves too, and the dispersion is larger.
        EXPECT_GT(mva.ratio(), 0.7);
        EXPECT_LT(mva.ratio(), 1.8);
        // The error of the sum is not the sum of the errors: the cost and
        // the benefit come from opposite scenarios.
        EXPECT_LT(fva.reported, fca.reported + fba.reported);
    }

    TEST(XvaReport, NoSpreadNoErrorAndFourTimesThePathsHalveIt)
    {
        const HullWhiteCurveModel m = model();
        XvaInputs in = inputs();
        const XvaReport none = xva_report(book(m, 2000), in);
        EXPECT_EQ(none.fca_error, 0.0);
        EXPECT_EQ(none.fba_error, 0.0);
        EXPECT_EQ(none.colva_error, 0.0);
        EXPECT_EQ(none.mva_error, 0.0);
        EXPECT_EQ(none.kva_error, 0.0);
        in.borrowing_spread = in.lending_spread = 0.005;
        const XvaReport small = xva_report(book(m, 4000), in);
        const XvaReport large = xva_report(book(m, 16000), in);
        EXPECT_NEAR(small.fca_error / large.fca_error, 2.0, 0.15);
        EXPECT_NEAR(small.exposure.pfe_error[20] / large.exposure.pfe_error[20], 2.0, 0.5);
        // Too few paths for two batches: no batch error, and no crash.
        EXPECT_EQ(xva_report(book(m, 12), in).exposure.epe_error, 0.0);
    }

    // ── Lot X5b: the margin from SIMM ────────────────────────────────────────

    TEST(XvaReport, TheInitialMarginCanBeSimmItselfOrARegressionStartedFromIt)
    {
        const HullWhiteCurveModel m = model();
        const auto simulate = [&](bool with_simm)
        {
            HullWhiteExposureEngine engine(m);
            const Real par =
                value_swap(make_swap(0.0, 10.0, 0.0, 1, 1), MultiCurve{m.discount(), m.projection()})
                    .par_rate;
            engine.add(make_swap(0.0, 10.0, par, 1, 1, 100.0, true));
            engine.add(make_swap(0.0, 10.0, par, 1, 1, 40.0, false));
            ExposureSimulationSettings settings;
            settings.paths = 5000;
            settings.keep_cashflows = true;
            settings.simm = with_simm;
            settings.grid.margin_period_of_risk = kTenDays;
            return engine.simulate(settings);
        };
        const ExposurePaths paths = simulate(true);
        XvaInputs in = margined_inputs();
        in.collateral.cashflows = MarginPeriodCashflows::Withheld;
        in.initial_margin = DimSettings{};

        const XvaReport regression = xva_report(paths, in);
        in.margin_model = XvaInputs::MarginModel::RegressionOnSimm;
        const XvaReport anchored = xva_report(paths, in);
        in.margin_model = XvaInputs::MarginModel::SimmPerPath;
        const XvaReport simm = xva_report(paths, in);

        // Both start from today's SIMM, which is not the model's own figure.
        EXPECT_DOUBLE_EQ(anchored.initial_margin_today, paths.simm_today.total());
        EXPECT_DOUBLE_EQ(simm.initial_margin_today, paths.simm_today.total());
        EXPECT_GT(paths.simm_today.total(), regression.initial_margin_today);
        // The anchored regression is the regression, scaled.
        const Real scale = paths.simm_today.total() / regression.initial_margin_today;
        EXPECT_NEAR(anchored.mva, scale * regression.mva, 1e-9 * std::abs(regression.mva));
        // SIMM per path and the regression started from it tell the same
        // story: the same margin today, a close cost of funding it.
        EXPECT_LT(simm.mva, 0.0);
        EXPECT_NEAR(simm.mva, anchored.mva, 0.15 * std::abs(anchored.mva));
        // A larger margin than the model's 99 %: even less exposure left.
        EXPECT_GT(simm.cva.value, regression.cva.value);
        EXPECT_LE(simm.cva.value, 0.0);
        // The shares of each trade are still computed.
        for (const TradeContribution &c : simm.contributions)
            EXPECT_TRUE(std::isfinite(c.incremental_cva));

        // SIMM is that of all the trades of a simulation that computed it.
        XvaInputs subset = in;
        subset.trades = {0};
        EXPECT_THROW(xva_report(paths, subset), InvalidInput);
        EXPECT_THROW(xva_report(simulate(false), in), InvalidInput);
    }

} // namespace quantModeling
