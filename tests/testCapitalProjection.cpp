#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/capital.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/regulatory/irb.hpp"
#include "quantModeling/risk/xva.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X5 of blueprint/wp/23-xva.md: regulatory capital projected on the
// simulated paths, and its cost (KVA).

namespace quantModeling
{
    namespace
    {
        constexpr Time kMpor = 10.0 / 250.0;
        constexpr Real kNotional = 100.0;

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.04));
        }

        InterestRateSwap par_swap(const HullWhiteCurveModel &m, Time tenor)
        {
            const Real par =
                value_swap(make_swap(0.0, tenor, 0.0, 1, 1), MultiCurve{m.discount(), m.projection()})
                    .par_rate;
            return make_swap(0.0, tenor, par, 1, 1, kNotional, true);
        }

        /// The 10-year payer swap of the simulation, as SA-CCR describes it.
        sa_ccr::Trade swap_trade(Time tenor = 10.0)
        {
            sa_ccr::Trade t;
            t.subclass = sa_ccr::SubClass::InterestRate;
            t.hedging_set = "USD";
            t.notional = kNotional;
            t.long_primary_risk_factor = true; // a payer swap gains when rates rise
            t.maturity = tenor;
            t.start = 0.0;
            t.end = tenor;
            return t;
        }

        CapitalInputs inputs()
        {
            CapitalInputs in;
            in.trades = {swap_trade()};
            in.pd = 0.01;
            in.lgd = 0.40;
            in.sector = ba_cva::Sector::Financial;
            in.quality = ba_cva::CreditQuality::InvestmentGrade;
            return in;
        }

        ExposurePaths simulate(const HullWhiteCurveModel &m, std::size_t paths = 5000)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_swap(m, 10.0));
            ExposureSimulationSettings s;
            s.paths = paths;
            s.keep_cashflows = true;
            s.grid.margin_period_of_risk = kMpor;
            return engine.simulate(s);
        }

        sa_ccr::MarginAgreement agreement()
        {
            sa_ccr::MarginAgreement a;
            a.margin_period_of_risk = kMpor;
            return a;
        }
    } // namespace

    // ── SA-CCR pieces ────────────────────────────────────────────────────────

    TEST(CapitalProjection, TheEadFromKnownAddOnsIsTheFullSaCcrCalculation)
    {
        for (const Real value : {-8.0, -1.0, 0.0, 3.0, 12.0})
        {
            sa_ccr::Trade trade = swap_trade();
            trade.market_value = value;
            sa_ccr::NettingSet open{{trade}, 0.0, std::nullopt};
            const Real add_on = sa_ccr::add_ons(open.trades).aggregate();
            EXPECT_DOUBLE_EQ(sa_ccr::exposure_at_default(value, add_on),
                             sa_ccr::exposure_at_default(open).ead);

            // Margined, with collateral held and a threshold.
            sa_ccr::MarginAgreement a = agreement();
            a.threshold = 2.0;
            a.minimum_transfer_amount = 0.5;
            sa_ccr::NettingSet margined{{trade}, 1.5, a};
            EXPECT_DOUBLE_EQ(
                sa_ccr::exposure_at_default(value - 1.5, add_on, a,
                                            sa_ccr::add_ons(margined.trades, a).aggregate()),
                sa_ccr::exposure_at_default(margined).ead);
        }
        // Nothing left to add on: alpha times the replacement cost.
        EXPECT_DOUBLE_EQ(sa_ccr::exposure_at_default(3.0, 0.0), sa_ccr::alpha * 3.0);
        EXPECT_EQ(sa_ccr::exposure_at_default(-3.0, 0.0), 0.0);
        EXPECT_THROW(sa_ccr::exposure_at_default(1.0, -1.0), InvalidInput);
    }

    TEST(CapitalProjection, ATradeAgesAndAnOptionBecomesItsUnderlying)
    {
        const sa_ccr::Trade swap = swap_trade();
        const auto later = aged_trade(swap, 4.0);
        ASSERT_TRUE(later.has_value());
        EXPECT_DOUBLE_EQ(later->maturity, 6.0);
        EXPECT_DOUBLE_EQ(later->end, 6.0);
        EXPECT_EQ(later->start, 0.0);
        EXPECT_FALSE(aged_trade(swap, 10.0).has_value());
        // Less time left, less supervisory duration, a smaller add-on.
        EXPECT_LT(sa_ccr::add_ons({*later}).aggregate(), sa_ccr::add_ons({swap}).aggregate());

        // A sold receiver swaption, 2 years into 8: a put sold, long rates.
        sa_ccr::Trade swaption = swap_trade();
        swaption.start = 2.0;
        swaption.option = sa_ccr::OptionTerms{sa_ccr::OptionType::Put, sa_ccr::OptionSide::Sold,
                                              0.04, 0.04, 2.0, 0.0};
        const auto before = aged_trade(swaption, 0.5);
        ASSERT_TRUE(before && before->option);
        EXPECT_DOUBLE_EQ(before->option->exercise, 1.5);
        EXPECT_DOUBLE_EQ(before->start, 1.5);
        const auto after = aged_trade(swaption, 3.0);
        ASSERT_TRUE(after.has_value());
        EXPECT_FALSE(after->option.has_value());
        EXPECT_TRUE(after->long_primary_risk_factor);
        swaption.option->side = sa_ccr::OptionSide::Bought;
        EXPECT_FALSE(aged_trade(swaption, 3.0)->long_primary_risk_factor);

        // Effective maturity: notional-weighted, floored at one year.
        sa_ccr::Trade big = swap_trade(4.0);
        big.notional = 3.0 * kNotional;
        EXPECT_DOUBLE_EQ(effective_maturity({swap, big}, 0.0), (10.0 + 3.0 * 4.0) / 4.0);
        EXPECT_DOUBLE_EQ(effective_maturity({swap, big}, 5.0), 5.0); // the 4-year swap is gone
        EXPECT_DOUBLE_EQ(effective_maturity({swap}, 9.5), 1.0);
        EXPECT_EQ(effective_maturity({swap}, 10.0), 0.0);
    }

    // ── The projection ───────────────────────────────────────────────────────

    TEST(CapitalProjection, TodaysCapitalIsTheRegulatoryCalculationAndItRunsOff)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m);
        const CapitalInputs in = inputs();
        const CapitalProfile capital = projected_capital(paths, in);

        // Today: SA-CCR on the trade as it is, then IRB and BA-CVA.
        sa_ccr::Trade trade = in.trades[0];
        trade.market_value = paths.trade_values_today[0];
        const Real ead = sa_ccr::exposure_at_default(sa_ccr::NettingSet{{trade}, 0.0, std::nullopt}).ead;
        EXPECT_DOUBLE_EQ(capital.ead_today, ead);
        // A 10-year netting set: the five-year cap applies to the IRB charge...
        EXPECT_DOUBLE_EQ(capital.default_capital_today,
                         irb::capital_requirement(0.01, 0.40, 5.0) * ead);
        // ...and not to BA-CVA.
        ba_cva::Counterparty c{in.sector, in.quality, {{ead, 10.0, false}}, {}};
        EXPECT_NEAR(capital.cva_capital_today, ba_cva::capital_reduced({c}), 1e-12 * ead);

        // A par swap: at first the exposure builds up, then the trade runs
        // off and the capital with it.
        const auto at = [&](Time t)
        {
            std::size_t i = 0;
            while (capital.times[i] < t - 1e-9)
                ++i;
            return i;
        };
        EXPECT_GT(capital.expected_ead[at(2.0)], capital.ead_today);
        EXPECT_GT(capital.expected_ead[at(5.0)], capital.expected_ead[at(8.0)]);
        EXPECT_GT(capital.expected_ead[at(8.0)], capital.expected_ead[at(9.5)]);
        EXPECT_EQ(capital.expected_ead.back(), 0.0);
        for (std::size_t i = 0; i < capital.times.size(); ++i)
        {
            ASSERT_GE(capital.discounted_default_capital[i], 0.0);
            ASSERT_DOUBLE_EQ(capital.discounted_capital[i], capital.discounted_default_capital[i] +
                                                                capital.discounted_cva_capital[i]);
        }

        // The PD floor of CRE32.4.
        CapitalInputs safe = in;
        safe.pd = 0.0;
        EXPECT_DOUBLE_EQ(projected_capital(paths, safe).default_capital_today,
                         irb::capital_requirement(irb_pd_floor, 0.40, 5.0) * ead);
    }

    TEST(CapitalProjection, KvaIsTheCostOfTheCapitalAndAnEmptyNettingSetHasNone)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m);
        const CapitalProfile capital = projected_capital(paths, inputs());
        const CreditCurve safe(0.0), risky(0.03);
        const Real cost = kva(capital.times, capital.discounted_capital, safe, safe, 0.10);
        EXPECT_LT(cost, 0.0);
        EXPECT_NEAR(kva(capital.times, capital.discounted_capital, safe, safe, 0.15), 1.5 * cost,
                    1e-12 * std::abs(cost));
        // The capital is released when either party defaults.
        EXPECT_GT(kva(capital.times, capital.discounted_capital, risky, safe, 0.10), cost);
        // Order of magnitude: 10 % a year on today's capital for a few years.
        const Real today = capital.default_capital_today + capital.cva_capital_today;
        EXPECT_GT(-cost, 0.10 * today * 2.0);
        EXPECT_LT(-cost, 0.10 * today * 12.0);

        // No trade: no exposure, no capital, no KVA.
        ExposurePaths empty = paths;
        std::fill(empty.trade_values[0].begin(), empty.trade_values[0].end(), 0.0);
        empty.trade_values_today = {0.0};
        CapitalInputs none = inputs();
        none.trades.clear();
        const CapitalProfile nothing = projected_capital(empty, none);
        EXPECT_EQ(nothing.ead_today, 0.0);
        for (const Real k : nothing.discounted_capital)
            ASSERT_EQ(k, 0.0);
        EXPECT_EQ(kva(nothing.times, nothing.discounted_capital, safe, safe, 0.10), 0.0);
    }

    TEST(CapitalProjection, MarginLowersTheCapital)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m);
        Csa csa;
        csa.margin_period_of_risk = kMpor;
        CollateralSettings withheld;
        withheld.cashflows = MarginPeriodCashflows::Withheld;
        const ExposurePaths collateralised = collateralise(paths, csa, {}, withheld);

        const CapitalInputs open = inputs();
        CapitalInputs margined = inputs();
        margined.margin = agreement();
        const CapitalProfile a = projected_capital(paths, open);
        const CapitalProfile b = projected_capital(collateralised, margined);
        const InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        const CapitalProfile c = projected_capital(collateralised, margined, &im);

        const CreditCurve safe(0.0);
        const Real open_kva = kva(a.times, a.discounted_capital, safe, safe, 0.10);
        const Real vm_kva = kva(b.times, b.discounted_capital, safe, safe, 0.10);
        const Real im_kva = kva(c.times, c.discounted_capital, safe, safe, 0.10);
        // Variation margin: the replacement cost goes, and the add-on is
        // scaled to ten days instead of a year.
        EXPECT_LT(b.ead_today, 0.5 * a.ead_today);
        EXPECT_GT(vm_kva, 0.5 * open_kva);
        EXPECT_LT(vm_kva, 0.0);
        // Initial margin over-collateralises: the multiplier falls below one,
        // towards its floor of 5 %.
        EXPECT_LT(c.ead_today, 0.5 * b.ead_today);
        EXPECT_GT(im_kva, 0.5 * vm_kva);
        EXPECT_LT(im_kva, 0.0);
        EXPECT_GE(c.ead_today, sa_ccr::alpha * sa_ccr::multiplier_floor *
                                   sa_ccr::add_ons(open.trades, margined.margin).aggregate() *
                                   (1.0 - 1e-12));
    }

    // ── The internal models method (CRE53) ───────────────────────────────────

    TEST(CapitalProjection, EffectiveEpeAndMaturityAreTheFormulasOfTheRule)
    {
        // A profile worked by hand, quarterly for a year then half-yearly.
        const std::vector<Time> times{0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
        const std::vector<Real> ee{4.0, 6.0, 5.0, 3.0, 2.0, 0.0};
        const std::vector<Real> flat(times.size(), 1.0);
        // Effective EE: 4, 6, 6, 6 — it never comes down. Its average over
        // the year: (4 + 6 + 6 + 6) / 4.
        const InternalModelExposure a = internal_model_exposure(times, ee, flat, 0.0, 1.0);
        EXPECT_DOUBLE_EQ(a.effective_epe, 5.5);
        EXPECT_DOUBLE_EQ(a.ead(), 1.4 * 5.5);
        // M: the first year's area plus the plain EE beyond, over the first
        // year's area: (5.5 + 2 × 0.5 + 0 × 0.5) / 5.5.
        EXPECT_DOUBLE_EQ(a.effective_maturity, 6.5 / 5.5);
        // The current exposure is the first Effective EE: above every EE of
        // the year, it is the whole year's.
        EXPECT_DOUBLE_EQ(internal_model_exposure(times, ee, flat, 0.0, 9.0).effective_epe, 9.0);

        // A step that straddles the year is cut at it: 2 for 0.6, then 4 for
        // 0.4 inside the year and 0.4 beyond.
        const InternalModelExposure cut =
            internal_model_exposure({0.6, 1.4}, {2.0, 4.0}, {1.0, 1.0}, 0.0, 0.0);
        EXPECT_DOUBLE_EQ(cut.effective_epe, 2.0 * 0.6 + 4.0 * 0.4);
        EXPECT_DOUBLE_EQ(cut.effective_maturity, (2.8 + 4.0 * 0.4) / 2.8);

        // Less than a year left: the average over what is left, and M at its
        // floor of one year.
        const InternalModelExposure brief =
            internal_model_exposure({0.25, 0.5}, {2.0, 4.0}, {1.0, 1.0}, 0.0, 3.0);
        EXPECT_DOUBLE_EQ(brief.effective_epe, (3.0 * 0.25 + 4.0 * 0.25) / 0.5);
        EXPECT_DOUBLE_EQ(brief.effective_maturity, 1.0);

        // Seen from a later date, time restarts there: from 0.5 with an
        // exposure of 6, the year runs to 1.5 and nothing is above 6.
        const InternalModelExposure later = internal_model_exposure(times, ee, flat, 0.5, ee[1]);
        EXPECT_DOUBLE_EQ(later.effective_epe, 6.0);
        EXPECT_DOUBLE_EQ(later.effective_maturity, 1.0); // 0 of EE in the last half-year
        // Nothing after the last date.
        EXPECT_EQ(internal_model_exposure(times, ee, flat, 2.0, 0.0).ead(), 0.0);

        // Discounting only weighs the maturity: the far exposure counts less.
        std::vector<Real> discount;
        for (const Time t : times)
            discount.push_back(std::exp(-0.05 * t));
        const InternalModelExposure d = internal_model_exposure(times, ee, discount, 0.0, 1.0);
        EXPECT_DOUBLE_EQ(d.effective_epe, 5.5);
        EXPECT_LT(d.effective_maturity, a.effective_maturity);
        EXPECT_GT(d.effective_maturity, 1.0);

        EXPECT_THROW(internal_model_exposure(times, {1.0}, flat, 0.0, 0.0), InvalidInput);
        EXPECT_THROW(internal_model_exposure(times, ee, flat, 0.0, -1.0), InvalidInput);
        EXPECT_THROW(internal_model_exposure(times, ee, flat, 0.3, 0.0), InvalidInput); // not a date
    }

    TEST(CapitalProjection, TheInternalModelNeedsNoDescriptionOfTheTrades)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m);
        CapitalInputs in = inputs();
        in.method = ExposureMethod::InternalModel;
        in.trades.clear(); // the simulation is the description
        const CapitalProfile capital = projected_capital(paths, in);

        // Today: 1.4 × the Effective EPE of the exposure statistics, computed
        // by other code (a par swap has no current exposure to start from).
        const ExposureStatistics stats = exposure_statistics(paths);
        EXPECT_NEAR(capital.ead_today, internal_model_alpha * stats.eepe, 1e-9 * capital.ead_today);
        EXPECT_GT(capital.ead_today, 0.0);
        // The charges on that exposure: IRB at the maturity of the rule,
        // BA-CVA without the cap and without the supervisory discount.
        const InternalModelExposure today =
            internal_model_exposure(stats.times, stats.ee, paths.discount, 0.0, 0.0);
        // M is a ratio of areas, not a date: a swap's exposure builds up
        // after the first year, so the whole area is many times the first
        // year's — more than the swap's ten years, and what the cap is for.
        EXPECT_GT(today.effective_maturity, 10.0);
        EXPECT_LT(today.effective_maturity, 20.0);
        EXPECT_NEAR(capital.default_capital_today,
                    irb::capital_requirement(0.01, 0.40, std::min(today.effective_maturity, 5.0)) *
                        capital.ead_today,
                    1e-9 * capital.default_capital_today);
        ba_cva::Counterparty c{ba_cva::Sector::Financial, ba_cva::CreditQuality::InvestmentGrade, {}};
        c.netting_sets.push_back({capital.ead_today, today.effective_maturity, true});
        EXPECT_NEAR(capital.cva_capital_today, ba_cva::capital_reduced({c}),
                    1e-9 * capital.cva_capital_today);

        // The profile runs off with the swap, and costs something.
        EXPECT_EQ(capital.expected_ead.back(), 0.0);
        const auto at = [&capital](Time t)
        {
            std::size_t i = 0;
            while (capital.times[i] < t - 1e-9)
                ++i;
            return i;
        };
        EXPECT_GT(capital.expected_ead[at(3.0)], capital.expected_ead[at(8.0)]);
        EXPECT_GT(capital.expected_ead[at(8.0)], capital.expected_ead[at(9.5)]);
        const CreditCurve safe(0.0);
        EXPECT_LT(kva(capital.times, capital.discounted_capital, safe, safe, 0.10), 0.0);

        // The same order of magnitude as the standardised approach, which
        // is built to be the more conservative of the two.
        const CapitalProfile standard = projected_capital(paths, inputs());
        EXPECT_LT(capital.ead_today, standard.ead_today);
        EXPECT_GT(capital.ead_today, 0.2 * standard.ead_today);
    }

    TEST(CapitalProjection, TheInternalModelSeesTheCollateral)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m);
        Csa csa;
        csa.margin_period_of_risk = kMpor;
        const ExposurePaths collateralised = collateralise(paths, csa);
        CapitalInputs in = inputs();
        in.method = ExposureMethod::InternalModel;
        const CapitalProfile open = projected_capital(paths, in);
        const CapitalProfile margined = projected_capital(collateralised, in);
        const InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        const CapitalProfile with_im = projected_capital(collateralised, in, &im);
        // Variation margin leaves the move of ten days; initial margin, sized
        // for 99 % of it, nearly nothing.
        EXPECT_LT(margined.ead_today, 0.3 * open.ead_today);
        EXPECT_GT(margined.ead_today, 0.0);
        EXPECT_LT(with_im.ead_today, 0.1 * margined.ead_today);
    }

    TEST(CapitalProjection, RejectsWhatItCannotUse)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 500);
        CapitalInputs in = inputs();
        in.pd = 1.0;
        EXPECT_THROW(projected_capital(paths, in), InvalidInput);
        in = inputs();
        in.lgd = 1.5;
        EXPECT_THROW(projected_capital(paths, in), InvalidInput);
        ExposurePaths two = paths;
        two.trade_values.push_back(two.trade_values[0]);
        two.trade_values_today.push_back(0.0);
        EXPECT_THROW(projected_capital(two, inputs()), InvalidInput);
        ExposurePaths historical = paths;
        historical.measure = ExposureMeasure::Historical;
        EXPECT_THROW(projected_capital(historical, inputs()), InvalidInput);
        // A margin that is not this cube's.
        InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        EXPECT_THROW(projected_capital(paths, inputs(), &im), InvalidInput);
        EXPECT_THROW(aged_trade(swap_trade(), -1.0), InvalidInput);
    }

} // namespace quantModeling
