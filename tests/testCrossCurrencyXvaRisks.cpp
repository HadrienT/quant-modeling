#include <gtest/gtest.h>

#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/engines/xva/xva_market_risks.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/instruments/rates/swap.hpp"
#include "quantModeling/models/rates/cross_currency_hull_white.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/xva.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

// blueprint/wp/23-xva.md §14.17: the adjoint of the two-currency exposure
// engine. As for one rate (testXvaRisks.cpp): the values of the run on
// doubles are the pricing engine's, and every adjoint sensitivity is the
// central difference of that run, bumped, on the same scenarios.

namespace quantModeling
{
    namespace
    {
        /// Everything a run depends on, so that any of it can be bumped.
        struct Market
        {
            std::vector<Time> pillars{1.0, 2.0, 5.0, 10.0};
            std::vector<Real> zero_d{0.038, 0.040, 0.042, 0.043};
            std::vector<Real> zero_f{0.022, 0.024, 0.026, 0.028};
            /// An asset instead of a second currency: the foreign curve is
            /// its dividend yield, flat, and its rate does not move.
            bool asset = false;
            Real dividend_yield = 0.02;
            Real a_d = 0.03, sigma_d = 0.010, a_f = 0.05, sigma_f = 0.008;
            Real spot = 1.10, sigma_s = 0.09;
            Real rho_df = 0.35, rho_ds = 0.25, rho_fs = -0.40;
            Real hazard_c = 0.03, hazard_i = 0.015;
            Real lgd_c = 0.6, lgd_i = 0.55, spread_b = 0.004, spread_l = 0.002;
        };

        DiscountCurve curve(const std::vector<Time> &pillars, const std::vector<Real> &zero)
        {
            std::vector<Real> dfs;
            for (std::size_t k = 0; k < pillars.size(); ++k)
                dfs.push_back(std::exp(-zero[k] * pillars[k]));
            return DiscountCurve(pillars, dfs, CurveExtrapolation::FlatForward);
        }

        CrossCurrencyHullWhiteModel model(const Market &m)
        {
            HullWhiteCurveModel domestic(m.a_d, m.sigma_d, curve(m.pillars, m.zero_d));
            if (m.asset)
                return CrossCurrencyHullWhiteModel(
                    domestic, HullWhiteCurveModel(m.a_f, m.sigma_f, DiscountCurve(m.dividend_yield)), m.spot,
                    m.sigma_s, {m.rho_df, m.rho_ds, m.rho_fs});
            return CrossCurrencyHullWhiteModel(domestic,
                                               HullWhiteCurveModel(m.a_f, m.sigma_f, curve(m.pillars, m.zero_f)),
                                               m.spot, m.sigma_s, {m.rho_df, m.rho_ds, m.rho_fs});
        }

        /// The inputs of a market in the order the run reports them.
        std::vector<Real *> factors(Market &m)
        {
            std::vector<Real *> out;
            for (Real &z : m.zero_d)
                out.push_back(&z);
            if (m.asset)
                out.push_back(&m.dividend_yield);
            else
                for (Real &z : m.zero_f)
                    out.push_back(&z);
            for (Real *x : {&m.a_d, &m.sigma_d, &m.a_f, &m.sigma_f, &m.spot, &m.sigma_s, &m.rho_df, &m.rho_ds,
                            &m.rho_fs, &m.hazard_c, &m.hazard_i, &m.lgd_c, &m.lgd_i, &m.spread_b, &m.spread_l})
                out.push_back(x);
            return out;
        }

        XvaRiskInputs inputs(const Market &m, bool csa = false)
        {
            XvaRiskInputs in;
            if (csa)
            {
                Csa terms;
                terms.threshold_counterparty = 2.0;
                terms.independent_amount = 0.5;
                in.csa = terms;
                in.cashflows = MarginPeriodCashflows::Withheld;
            }
            in.counterparty = CreditCurve(m.hazard_c);
            in.own = CreditCurve(m.hazard_i);
            in.lgd_counterparty = m.lgd_c;
            in.lgd_own = m.lgd_i;
            in.borrowing_spread = m.spread_b;
            in.lending_spread = m.spread_l;
            return in;
        }

        ExposureSimulationSettings settings(std::size_t paths)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 7;
            return s;
        }

        VanillaOption option(Real strike, Time expiry, bool call, Real units)
        {
            return VanillaOption(
                std::make_shared<PlainVanillaPayoff>(call ? OptionType::Call : OptionType::Put, strike),
                std::make_shared<EuropeanExercise>(expiry), units);
        }

        /// The trades are written in numbers, not at the market: a bumped
        /// market leaves them alone.
        using Book = std::function<void(CrossCurrencyExposureEngine &)>;

        /// Forwards and cross-currency swaps both ways: a netting set whose
        /// value changes sign.
        void fx_book(CrossCurrencyExposureEngine &engine)
        {
            engine.add(FXForward(1.12, 3.0, 100.0));
            engine.add(FXForward(1.20, 5.0, 80.0), -1.0);
            engine.add(CrossCurrencySwap(5.0, 100.0, 108.0, 0.025, 0.041));
            engine.add(CrossCurrencySwap(8.0, 60.0, 68.0, 0.027, 0.043, 1, false));
        }

        /// The same with options on the exchange rate, bought and sold.
        void option_book(CrossCurrencyExposureEngine &engine)
        {
            engine.add(FXForward(1.15, 4.0, 100.0), -1.0);
            engine.add(option(1.15, 2.0, true, 150.0));
            engine.add(option(1.05, 4.0, false, 120.0), -1.0);
            engine.add(option(1.25, 6.0, true, 90.0));
        }

        /// Options on an asset around 100, and nothing else.
        void asset_book(CrossCurrencyExposureEngine &engine)
        {
            engine.add(option(105.0, 2.0, true, 3.0));
            engine.add(option(90.0, 3.0, false, 4.0), -1.0);
            engine.add(option(100.0, 5.0, false, 2.0));
        }

        /// Everything at once: swaps and swaptions of both currencies next to
        /// a forward, a cross-currency swap and an option on the exchange
        /// rate. Fixed rates are numbers, not par rates.
        void mixed_book(CrossCurrencyExposureEngine &engine)
        {
            engine.add_domestic(make_swap(0.0, 6.0, 0.039, 1, 2, 120.0, true));
            engine.add_domestic(make_swap(1.0, 4.0, 0.043, 1, 1, 90.0, false), 1.5);
            engine.add_foreign(make_swap(0.0, 5.0, 0.027, 1, 1, 100.0, false));
            engine.add_domestic(Swaption{make_swap(2.0, 5.0, 0.041, 1, 2, 150.0, true), 2.0});
            engine.add_foreign(Swaption{make_swap(1.0, 4.0, 0.025, 1, 1, 110.0, false), 1.0}, -1.0);
            engine.add(FXForward(1.14, 3.0, 40.0));
            engine.add(CrossCurrencySwap(5.0, 50.0, 54.0, 0.025, 0.041, 1, false));
            engine.add(option(1.12, 4.0, true, 60.0));
        }

        /// A swap and a swaption in dollars next to options on an asset: the
        /// netting set of a portfolio with equity options.
        void asset_and_rates_book(CrossCurrencyExposureEngine &engine)
        {
            engine.add_domestic(make_swap(0.0, 5.0, 0.040, 1, 1, 400.0, true));
            engine.add_domestic(Swaption{make_swap(1.0, 4.0, 0.042, 1, 1, 300.0, false), 1.0});
            engine.add(option(105.0, 2.0, true, 3.0));
            engine.add(option(95.0, 4.0, false, 4.0), -1.0);
        }

        Market asset_market()
        {
            Market m;
            m.asset = true;
            m.spot = 100.0;
            m.sigma_s = 0.22;
            m.rho_ds = 0.30;
            return m;
        }

        /// A bumped run smooths exactly as the base run did: its widths.
        XvaValues values(const Market &m, const Book &book, std::size_t paths, bool csa = false,
                         const std::vector<Real> &widths = {})
        {
            const CrossCurrencyHullWhiteModel cc = model(m);
            CrossCurrencyExposureEngine engine(cc);
            book(engine);
            XvaRiskInputs in = inputs(m, csa);
            in.smoothing_widths = widths;
            return engine.xva_values(settings(paths), in);
        }

        XvaRisks risks(const Market &m, const Book &book, std::size_t paths, ThreadPool *pool = nullptr,
                       bool csa = false)
        {
            const CrossCurrencyHullWhiteModel cc = model(m);
            CrossCurrencyExposureEngine engine(cc);
            book(engine);
            return engine.xva_risks(settings(paths), inputs(m, csa), pool);
        }

        /// Every adjoint sensitivity against a central difference of the same
        /// estimator, on the same scenarios.
        void expect_adjoint_is_bump(const Market &base, const Book &book, std::size_t paths, bool csa = false)
        {
            const XvaRisks adjoint = risks(base, book, paths, nullptr, csa);
            Market probe = base;
            ASSERT_EQ(factors(probe).size(), adjoint.factors.size());
            std::size_t compared = 0;
            for (std::size_t j = 0; j < adjoint.factors.size(); ++j)
            {
                Market up = base, down = base;
                const Real level = *factors(up)[j];
                // (a flat curve's rate is read back from its discount factor)
                EXPECT_NEAR(level, adjoint.levels[j], 1e-15) << adjoint.labels[j];
                // A small bump: the estimator has a kink wherever a path's
                // exposure changes sign.
                const Real h = 2e-7 * std::max(std::abs(level), 0.01);
                *factors(up)[j] += h;
                *factors(down)[j] -= h;
                const XvaValues high = values(up, book, paths, csa, adjoint.smoothing_widths);
                const XvaValues low = values(down, book, paths, csa, adjoint.smoothing_widths);
                for (std::size_t o = 0; o < kXvaOutputs; ++o)
                {
                    const Real bump = (high.values[o].value - low.values[o].value) / (2.0 * h);
                    const Real risk = adjoint.risks[o * adjoint.factors.size() + j];
                    EXPECT_NEAR(risk, bump, 2e-5 * std::abs(bump) + 1e-5 * std::abs(adjoint.values[o].value))
                        << adjoint.labels[j] << ", output " << o;
                    if (std::abs(bump) > 0.0)
                        ++compared;
                }
            }
            // Not a comparison of zeros.
            EXPECT_GT(compared, 60u);
        }
    } // namespace

    TEST(CrossCurrencyXvaRisks, TheValuesAreThePricingEngines)
    {
        struct Case
        {
            Market market;
            Book book;
            bool csa;
        };
        const std::vector<Case> cases{{Market{}, option_book, false},
                                      {asset_market(), asset_book, false},
                                      {Market{}, mixed_book, false},
                                      {Market{}, mixed_book, true},
                                      {asset_market(), asset_and_rates_book, true}};
        for (const Case &c : cases)
        {
            const Market &m = c.market;
            XvaRiskInputs in = inputs(m, c.csa);
            // The pricing engine exercises on the sign of the swap: no ramp.
            in.exercise_smoothing = 0.0;
            const CrossCurrencyHullWhiteModel cc = model(m);
            CrossCurrencyExposureEngine engine(cc);
            c.book(engine);
            const XvaValues direct = engine.xva_values(settings(3000), in);

            ExposureSimulationSettings s = settings(3000);
            s.keep_cashflows = true;
            if (c.csa)
                s.grid.margin_period_of_risk = in.csa->margin_period_of_risk;
            const ExposurePaths cube = engine.simulate(s);
            CollateralSettings collateral;
            collateral.cashflows = in.cashflows;
            const ExposureProfile profile = c.csa
                                                ? exposure_profile(collateralise(cube, *in.csa, {}, collateral))
                                                : exposure_profile(cube);
            const auto near = [](Real a, Real b)
            { EXPECT_NEAR(a, b, 1e-9 * std::abs(b)); };
            near(direct[XvaOutput::Cva].value, cva_bilateral(profile, in.counterparty, in.own, m.lgd_c));
            near(direct[XvaOutput::Dva].value, dva(profile, in.counterparty, in.own, m.lgd_i));
            near(direct[XvaOutput::Fca].value, fca(profile, in.counterparty, in.own, m.spread_b));
            near(direct[XvaOutput::Fba].value, fba(profile, in.counterparty, in.own, m.spread_l));
            near(direct[XvaOutput::CvaUnilateral].value, cva_unilateral(profile, in.counterparty, m.lgd_c));
            EXPECT_LT(direct[XvaOutput::Cva].value, 0.0);
            EXPECT_GT(direct[XvaOutput::Dva].value, 0.0);
            EXPECT_GT(direct[XvaOutput::Cva].error, 0.0);
        }
    }

    TEST(CrossCurrencyXvaRisks, TheAdjointRunGivesTheValuesOfTheDoubleRun)
    {
        const Market m;
        const XvaValues plain = values(m, option_book, 1000);
        const XvaRisks adjoint = risks(m, option_book, 1000);
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
            EXPECT_NEAR(adjoint.values[o].value, plain.values[o].value, 1e-12 * std::abs(plain.values[o].value));
    }

    TEST(CrossCurrencyXvaRisks, OnForwardsAndCrossCurrencySwapsEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(Market{}, fx_book, 400);
    }

    TEST(CrossCurrencyXvaRisks, OnOptionsEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(Market{}, option_book, 400);
    }

    TEST(CrossCurrencyXvaRisks, OnAnAssetEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(asset_market(), asset_book, 400);
    }

    TEST(CrossCurrencyXvaRisks, OnSwapsAndSwaptionsOfBothCurrenciesEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(Market{}, mixed_book, 256);
        // One width per rate trade, domestic ones first: the two swaptions
        // are smoothed, the swaps are not.
        const XvaRisks r = risks(Market{}, mixed_book, 64);
        ASSERT_EQ(r.smoothing_widths.size(), 5u);
        EXPECT_EQ(r.smoothing_widths[0], 0.0);
        EXPECT_EQ(r.smoothing_widths[1], 0.0);
        EXPECT_GT(r.smoothing_widths[2], 0.0);
        EXPECT_EQ(r.smoothing_widths[3], 0.0);
        EXPECT_GT(r.smoothing_widths[4], 0.0);
    }

    TEST(CrossCurrencyXvaRisks, UnderACsaEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(Market{}, mixed_book, 256, true);
        expect_adjoint_is_bump(Market{}, option_book, 256, true);
    }

    TEST(CrossCurrencyXvaRisks, OnAnAssetWithRateTradesEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_bump(asset_market(), asset_and_rates_book, 256);
        expect_adjoint_is_bump(asset_market(), asset_and_rates_book, 256, true);
    }

    TEST(CrossCurrencyXvaRisks, TheSignsAndTheErrorsMakeSense)
    {
        // Bought calls on an asset: the bank is owed more when the asset
        // rises or moves more.
        const Market m = asset_market();
        const Book calls = [](CrossCurrencyExposureEngine &engine)
        { engine.add(option(100.0, 3.0, true, 5.0)); };
        const XvaRisks r = risks(m, calls, 4000);
        const auto at = [&](XvaRiskFactor factor)
        {
            const auto it = std::find(r.factors.begin(), r.factors.end(), factor);
            return r.risk(XvaOutput::Cva, static_cast<std::size_t>(it - r.factors.begin()));
        };
        // CVA is a cost, negative: a larger exposure makes it more negative.
        EXPECT_LT(at(XvaRiskFactor::Spot).value, 0.0);
        EXPECT_LT(at(XvaRiskFactor::SpotVolatility).value, 0.0);
        // A higher dividend yield lowers the forward: a call is worth less.
        EXPECT_GT(at(XvaRiskFactor::ForeignZeroRate).value, 0.0);
        EXPECT_LT(at(XvaRiskFactor::CounterpartyHazard).value, 0.0);
        // Linear in the loss given default; a bought option is never owed.
        EXPECT_NEAR(at(XvaRiskFactor::CounterpartyLgd).value, r[XvaOutput::Cva].value / m.lgd_c,
                    1e-9 * std::abs(r[XvaOutput::Cva].value));
        EXPECT_EQ(r[XvaOutput::Dva].value, 0.0);
        // Each sensitivity has an error, well inside its value where it matters.
        for (const XvaRiskFactor factor : {XvaRiskFactor::Spot, XvaRiskFactor::SpotVolatility})
        {
            EXPECT_GT(at(factor).error, 0.0);
            EXPECT_LT(at(factor).error, 0.2 * std::abs(at(factor).value));
        }
        // The delta of the CVA is the CVA of the delta: per unit of spot, the
        // order of magnitude of CVA / spot.
        EXPECT_LT(std::abs(at(XvaRiskFactor::Spot).value), 20.0 * std::abs(r[XvaOutput::Cva].value) / m.spot);
    }

    TEST(CrossCurrencyXvaRisks, TheResultDoesNotDependOnTheThreads)
    {
        const Market m;
        const XvaRisks alone = risks(m, option_book, 500);
        ThreadPool pool;
        pool.start(3);
        const XvaRisks many = risks(m, option_book, 500, &pool);
        ASSERT_EQ(alone.risks.size(), many.risks.size());
        for (std::size_t k = 0; k < alone.risks.size(); ++k)
            EXPECT_DOUBLE_EQ(alone.risks[k], many.risks[k]);
    }

    TEST(CrossCurrencyXvaRisks, AtAFixedMarketVolatilityTheRatesMoveTheSpotVolatilityWithThem)
    {
        // The spot volatility is not quoted: it is what gives an option of
        // three years its market volatility. With that volatility held
        // fixed, a bump of the rate model's σ moves σ_S too.
        const Time expiry = 3.0;
        const Real quoted = 0.24;
        const auto market = [&](Real sigma_d, Real vol)
        {
            Market m = asset_market();
            m.sigma_d = sigma_d;
            // As make_asset_model: a yield that does not move.
            m.sigma_f = kDeterministicRateVolatility;
            m.a_f = m.a_d;
            m.rho_df = m.rho_fs = 0.0;
            m.sigma_s = calibrated_fx_volatility(HullWhiteCurveModel(m.a_d, m.sigma_d, DiscountCurve(0.0)),
                                                 HullWhiteCurveModel(m.a_f, m.sigma_f, DiscountCurve(0.0)),
                                                 {m.rho_df, m.rho_ds, m.rho_fs}, expiry, vol);
            return m;
        };
        const Market base = market(0.010, quoted);
        EXPECT_LT(base.sigma_s, quoted); // the rate adds to the forward's volatility
        const std::size_t paths = 256;
        XvaRisks r = risks(base, asset_and_rates_book, paths);
        const std::vector<Real> widths = r.smoothing_widths;
        const auto find = [&](XvaRiskFactor factor)
        { return static_cast<std::size_t>(std::find(r.factors.begin(), r.factors.end(), factor) - r.factors.begin()); };
        const std::size_t sigma = find(XvaRiskFactor::Sigma), vol = find(XvaRiskFactor::SpotVolatility);
        const Real before = r.risk(XvaOutput::Cva, sigma).value;
        const CrossCurrencyHullWhiteModel cc = model(base);
        fold_spot_volatility_calibration(r, cc, expiry, quoted);
        EXPECT_NE(r.risk(XvaOutput::Cva, sigma).value, before);
        EXPECT_EQ(r.labels[vol], "market volatility of the spot");
        EXPECT_DOUBLE_EQ(r.levels[vol], quoted);

        const auto cva = [&](const Market &m)
        { return values(m, asset_and_rates_book, paths, false, widths)[XvaOutput::Cva].value; };
        const Real h = 1e-7;
        const Real to_sigma = (cva(market(0.010 + h, quoted)) - cva(market(0.010 - h, quoted))) / (2.0 * h);
        const Real to_vol = (cva(market(0.010, quoted + h)) - cva(market(0.010, quoted - h))) / (2.0 * h);
        EXPECT_NEAR(r.risk(XvaOutput::Cva, sigma).value, to_sigma, 1e-4 * std::abs(to_sigma));
        EXPECT_NEAR(r.risk(XvaOutput::Cva, vol).value, to_vol, 1e-4 * std::abs(to_vol));
        EXPECT_GT(r.risk(XvaOutput::Cva, vol).error, 0.0);
        // One factor as a quote: its label, its level, every adjustment.
        const XvaQuoteRisk q = factor_risk(r, vol);
        EXPECT_EQ(q.label, r.labels[vol]);
        EXPECT_DOUBLE_EQ(q.risk[0].value, r.risk(XvaOutput::Cva, vol).value);
        // Not the calibration to this quote: refused.
        EXPECT_THROW(fold_spot_volatility_calibration(r, cc, expiry, 0.30), InvalidInput);
    }

    TEST(CrossCurrencyXvaRisks, RefusesWhatIsNotARun)
    {
        const Market m;
        const CrossCurrencyHullWhiteModel cc = model(m);
        EXPECT_THROW(CrossCurrencyExposureEngine(cc).xva_values(settings(100), inputs(m)), InvalidInput);
        CrossCurrencyExposureEngine engine(cc);
        mixed_book(engine);
        EXPECT_THROW(engine.xva_risks(settings(0), inputs(m)), InvalidInput);
        // One smoothing width per rate trade, or none.
        XvaRiskInputs widths = inputs(m);
        widths.smoothing_widths = {0.1, 0.2};
        EXPECT_THROW(engine.xva_values(settings(100), widths), InvalidInput);
        XvaRiskInputs lgd = inputs(m);
        lgd.lgd_counterparty = 1.5;
        EXPECT_THROW(engine.xva_values(settings(100), lgd), InvalidInput);
    }

} // namespace quantModeling
