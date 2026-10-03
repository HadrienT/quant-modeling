#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/models/rates/cross_currency_hull_white.hpp"
#include "quantModeling/risk/xva_report.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X10 of blueprint/wp/23-xva.md: two currencies. A Hull-White rate in
// each and a lognormal exchange rate, simulated exactly under the domestic
// forward measure; the cube is the one-currency engine's.

namespace quantModeling
{
    namespace
    {
        using Correlations = CrossCurrencyHullWhiteModel::Correlations;

        /// USD at 4 %, EUR at 2.5 %, 1.10 dollars per euro, 9 % of FX
        /// volatility, and correlations far enough from zero for every
        /// cross term to matter.
        CrossCurrencyHullWhiteModel model(Correlations rho = {0.35, 0.25, -0.40})
        {
            return CrossCurrencyHullWhiteModel(HullWhiteCurveModel(0.03, 0.010, DiscountCurve(0.04)),
                                               HullWhiteCurveModel(0.05, 0.008, DiscountCurve(0.025)),
                                               1.10, 0.09, rho);
        }

        InterestRateSwap swap(const HullWhiteCurveModel &m, Time tenor, Real rate_shift, bool payer,
                              Real notional = 100.0)
        {
            const MultiCurve curves{m.discount(), m.projection()};
            const Real par = value_swap(make_swap(0.0, tenor, 0.0), curves).par_rate;
            return make_swap(0.0, tenor, par + rate_shift, 1, 4, notional, payer);
        }

        /// The annual coupon that makes a bond of this curve worth par.
        Real par_coupon(const DiscountCurve &curve, Time maturity)
        {
            Real annuity = 0.0;
            for (int k = 1; k <= static_cast<int>(std::lround(maturity)); ++k)
                annuity += curve.discount(static_cast<Real>(k));
            return (1.0 - curve.discount(maturity)) / annuity;
        }

        /// A swap of 100 euros against their value in dollars today, both
        /// legs at par: worth zero.
        CrossCurrencySwap par_ccs(const CrossCurrencyHullWhiteModel &m, Time maturity)
        {
            return CrossCurrencySwap(maturity, 100.0, 100.0 * m.spot(),
                                     par_coupon(m.foreign().discount(), maturity),
                                     par_coupon(m.domestic().discount(), maturity));
        }

        ExposureSimulationSettings settings(std::size_t paths, bool cashflows = true)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.keep_cashflows = cashflows;
            return s;
        }

        /**
         * A price is a martingale once discounted, its cash flows put back:
         *
         *   V(0) = E[ D(t) V(t) + Σ_{s <= t} D(s) CF(s) ]   for every t.
         *
         * Returns the largest gap over the dates, in standard errors.
         */
        Real martingale_gap(const ExposurePaths &cube, std::size_t trade)
        {
            const std::size_t n = cube.dates(), N = cube.paths;
            std::vector<Real> sum(n, 0.0), sum2(n, 0.0);
            for (std::size_t p = 0; p < N; ++p)
            {
                Real paid = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Real w = cube.discount_weight[p * n + i];
                    paid += w * cube.trade_cashflows[trade][p * n + i];
                    const Real x = paid + w * cube.trade_values[trade][p * n + i];
                    sum[i] += x;
                    sum2[i] += x * x;
                }
            }
            Real worst = 0.0;
            const Real count = static_cast<Real>(N);
            for (std::size_t i = 0; i < n; ++i)
            {
                const Real mean = sum[i] / count;
                const Real error = std::sqrt(std::max(sum2[i] / count - mean * mean, 0.0) / (count - 1.0));
                worst = std::max(worst, std::abs(mean - cube.trade_values_today[trade]) / error);
            }
            return worst;
        }
    } // namespace

    // ── The model ────────────────────────────────────────────────────────────

    TEST(CrossCurrencyModel, TheForwardIsCoveredInterestParity)
    {
        const CrossCurrencyHullWhiteModel m = model();
        EXPECT_DOUBLE_EQ(m.forward_fx(5.0), 1.10 * std::exp(-0.025 * 5.0) / std::exp(-0.04 * 5.0));
        // The euro yields less: it trades at a forward premium.
        EXPECT_GT(m.forward_fx(5.0), m.spot());
        // The spot read back from a state at rest is the forward of that date.
        EXPECT_NEAR(m.spot_at(1e-12, 10.0, 0.0, 0.0, 0.0), m.spot(), 1e-9);
    }

    TEST(CrossCurrencyModel, TheImpliedVolatilityCarriesTheVolatilityOfBothRates)
    {
        // Rates that barely move: the forward has the spot's volatility.
        const CrossCurrencyHullWhiteModel still(HullWhiteCurveModel(0.03, 1e-7, DiscountCurve(0.04)),
                                                HullWhiteCurveModel(0.05, 1e-7, DiscountCurve(0.025)),
                                                1.10, 0.09, Correlations{});
        EXPECT_NEAR(still.fx_implied_volatility(10.0), 0.09, 1e-6);
        // Uncorrelated rates that move add their variance, more so with the
        // expiry: G(u, T) grows with T - u.
        const CrossCurrencyHullWhiteModel m = model(Correlations{});
        EXPECT_GT(m.fx_implied_volatility(1.0), 0.09);
        EXPECT_GT(m.fx_implied_volatility(10.0), m.fx_implied_volatility(1.0));
        // ...by an amount known in closed form at short expiry: the bonds'
        // volatilities are σ G ≈ σ (T - u), so the added variance is
        // (σ_d² + σ_f²) T³ / 3 to first order in a.
        const Real T = 0.25;
        const Real added = (0.010 * 0.010 + 0.008 * 0.008) * T * T * T / 3.0;
        EXPECT_NEAR(m.fx_implied_volatility(T) * m.fx_implied_volatility(T) * T, 0.09 * 0.09 * T + added,
                    0.02 * added);
        // The forward is the spot times the foreign bond over the domestic
        // one. A spot that rises with domestic rates rises as the domestic
        // bond falls: the forward moves more. With foreign rates, less.
        EXPECT_GT(model({0.0, 0.5, 0.0}).fx_implied_volatility(5.0), m.fx_implied_volatility(5.0));
        EXPECT_LT(model({0.0, 0.0, 0.5}).fx_implied_volatility(5.0), m.fx_implied_volatility(5.0));
    }

    TEST(CrossCurrencyModel, RefusesWhatIsNotAModel)
    {
        const HullWhiteCurveModel usd(0.03, 0.01, DiscountCurve(0.04)), eur(0.05, 0.008, DiscountCurve(0.025));
        EXPECT_THROW(CrossCurrencyHullWhiteModel(usd, eur, 0.0, 0.09, {}), InvalidInput);
        EXPECT_THROW(CrossCurrencyHullWhiteModel(usd, eur, 1.1, 0.0, {}), InvalidInput);
        EXPECT_THROW(CrossCurrencyHullWhiteModel(usd, eur, 1.1, 0.09, {1.0, 0.0, 0.0}), InvalidInput);
        // Each pair is a correlation, the three together are not.
        EXPECT_THROW(CrossCurrencyHullWhiteModel(usd, eur, 1.1, 0.09, {0.9, 0.9, -0.9}), InvalidInput);
        const CrossCurrencyHullWhiteModel m = model();
        EXPECT_THROW(m.transition(1.0, 1.0, 5.0), InvalidInput);
        EXPECT_THROW(m.transition(1.0, 2.0, 1.5), InvalidInput);
        EXPECT_THROW(m.fx_implied_volatility(0.0), InvalidInput);
    }

    TEST(CrossCurrencyModel, TheDomesticRateMovesAsItDoesAlone)
    {
        const CrossCurrencyHullWhiteModel m = model();
        const auto joint = m.transition(0.5, 0.75, 10.0);
        const auto alone = m.domestic().transition(0.5, 0.75, 10.0);
        EXPECT_EQ(joint.decay[0], alone.decay);
        EXPECT_EQ(joint.drift[0], alone.drift);
        EXPECT_EQ(joint.cholesky[0][0], std::sqrt(alone.variance));
        // Two steps make the variance of one twice as long, for the factor
        // whose increments add up: the forward exchange rate.
        const auto whole = m.transition(0.5, 1.0, 10.0), second = m.transition(0.75, 1.0, 10.0);
        const auto variance_z = [](const CrossCurrencyHullWhiteModel::Transition &t)
        {
            return t.cholesky[2][0] * t.cholesky[2][0] + t.cholesky[2][1] * t.cholesky[2][1] +
                   t.cholesky[2][2] * t.cholesky[2][2];
        };
        EXPECT_NEAR(variance_z(whole), variance_z(joint) + variance_z(second), 1e-15);
        EXPECT_NEAR(whole.drift[2], joint.drift[2] + second.drift[2], 1e-15);
        // The forward is a martingale: E[exp(drift + ξ)] = 1 exactly.
        EXPECT_NEAR(whole.drift[2], -0.5 * variance_z(whole), 1e-17);
    }

    // ── The engine ───────────────────────────────────────────────────────────

    TEST(CrossCurrencyEngine, DomesticTradesHaveTheOneCurrencyCubeBitForBit)
    {
        const CrossCurrencyHullWhiteModel m = model();
        const InterestRateSwap payer = swap(m.domestic(), 10.0, 0.002, true);
        const InterestRateSwap forward_swap = make_swap(2.0, 5.0, 0.04, 1, 4, 100.0, true);
        const Swaption swaption(forward_swap, 2.0);

        HullWhiteExposureEngine alone(m.domestic());
        alone.add(payer);
        alone.add(swaption, -1.0);
        CrossCurrencyExposureEngine both(m);
        both.add_domestic(payer);
        both.add_domestic(swaption, -1.0);

        const ExposurePaths a = alone.simulate(settings(500));
        const ExposurePaths b = both.simulate(settings(500));
        ASSERT_EQ(a.times, b.times);
        EXPECT_EQ(a.discount, b.discount);
        EXPECT_EQ(a.discount_weight, b.discount_weight);
        EXPECT_EQ(a.trade_values_today, b.trade_values_today);
        EXPECT_EQ(a.trade_values, b.trade_values);
        EXPECT_EQ(a.trade_cashflows, b.trade_cashflows);
    }

    TEST(CrossCurrencyEngine, AnFxForwardIsWorthParityAndStaysAMartingale)
    {
        const CrossCurrencyHullWhiteModel m = model();
        const Time T = 5.0;
        const Real K = 1.05; // below the forward: worth something today
        CrossCurrencyExposureEngine engine(m);
        engine.add(FXForward(K, T, 100.0));
        engine.add(FXForward(m.forward_fx(T), T, 100.0));
        const ExposurePaths cube = engine.simulate(settings(40000));

        // Covered interest parity: N (S0 P_f(0, T) - K P_d(0, T)).
        EXPECT_NEAR(cube.trade_values_today[0],
                    100.0 * (1.10 * std::exp(-0.025 * T) - K * std::exp(-0.04 * T)), 1e-10);
        // Struck at the forward it is worth nothing.
        EXPECT_NEAR(cube.trade_values_today[1], 0.0, 1e-10);
        // And at every date its discounted value is today's: the drift of
        // the spot, of both rates and the change of measure are all in it.
        EXPECT_LT(martingale_gap(cube, 0), 4.0);
        EXPECT_LT(martingale_gap(cube, 1), 4.0);
        // Nothing is paid before the delivery, everything then.
        const std::size_t n = cube.dates();
        for (std::size_t i = 0; i + 1 < n; ++i)
            ASSERT_EQ(cube.trade_cashflows[0][i], 0.0);
        EXPECT_NE(cube.trade_cashflows[0][n - 1], 0.0);
        EXPECT_EQ(cube.trade_values[0][n - 1], 0.0);
    }

    TEST(CrossCurrencyEngine, AForeignSwapConvertedAtTheSpotIsAMartingale)
    {
        // The test of the quanto drift: the euro swap is valued on the euro
        // rate, which the dollar measure sees with a drift -ρ_fS σ_f σ_S.
        // Without it the discounted value drifts away from today's by a
        // dozen standard errors at these correlations.
        const CrossCurrencyHullWhiteModel m = model({0.35, 0.25, -0.60});
        const InterestRateSwap euro_swap = swap(m.foreign(), 10.0, 0.004, true);
        CrossCurrencyExposureEngine engine(m);
        engine.add_foreign(euro_swap);
        engine.add_domestic(swap(m.domestic(), 10.0, -0.003, false));
        const ExposurePaths cube = engine.simulate(settings(40000));

        HullWhiteExposureEngine euros(m.foreign());
        euros.add(euro_swap);
        const Real value_in_euros = euros.simulate(settings(8)).trade_values_today[0];
        EXPECT_NE(value_in_euros, 0.0);
        EXPECT_DOUBLE_EQ(cube.trade_values_today[0], m.spot() * value_in_euros);
        EXPECT_LT(martingale_gap(cube, 0), 4.0);
        EXPECT_LT(martingale_gap(cube, 1), 4.0);
    }

    TEST(CrossCurrencyEngine, TheSpotHasItsForwardAsMeanAndTheImpliedVolatility)
    {
        const CrossCurrencyHullWhiteModel m = model();
        CrossCurrencyExposureEngine engine(m);
        const Time T = 4.0;
        engine.add(FXForward(1.10, T));
        CrossCurrencyScenarios scenarios;
        const std::size_t N = 40000;
        const ExposurePaths cube = engine.simulate(settings(N, false), nullptr, &scenarios);
        const std::size_t n = cube.dates();
        // At the horizon the measure is the T-forward one: plain statistics.
        Real mean = 0.0, log_mean = 0.0, log_square = 0.0;
        for (std::size_t p = 0; p < N; ++p)
        {
            const Real s = scenarios.spot[p * n + n - 1];
            mean += s / N;
            log_mean += std::log(s) / N;
            log_square += std::log(s) * std::log(s) / N;
        }
        const Real variance = log_square - log_mean * log_mean;
        const Real implied = m.fx_implied_volatility(T);
        EXPECT_NEAR(mean, m.forward_fx(T), 4.0 * m.forward_fx(T) * implied * std::sqrt(T / N));
        // The variance of a sample of N normals is known to sqrt(2 / N).
        EXPECT_NEAR(variance, implied * implied * T, 4.0 * implied * implied * T * std::sqrt(2.0 / N));
        // Before the horizon the spot is not the simulated factor: it is
        // read back through both bonds. Its discounted mean is the foreign
        // bond's price, S0 P_f(0, t).
        const std::size_t i = n / 2;
        Real sum = 0.0, sum2 = 0.0;
        for (std::size_t p = 0; p < N; ++p)
        {
            const Real x = cube.discount_weight[p * n + i] * scenarios.spot[p * n + i];
            sum += x;
            sum2 += x * x;
        }
        const Real error = std::sqrt((sum2 / N - sum / N * sum / N) / N);
        EXPECT_NEAR(sum / N, m.spot() * m.foreign().discount().discount(cube.times[i]), 4.0 * error);
    }

    TEST(CrossCurrencyEngine, TheFactorsHaveTheirJointLawAtEveryDate)
    {
        // What a martingale test cannot see: how the three factors move
        // together. Variances and covariances of Gaussian factors with
        // deterministic volatilities do not depend on the measure, so they
        // are known in closed form at every date, not only at the horizon.
        const CrossCurrencyHullWhiteModel m = model();
        CrossCurrencyExposureEngine engine(m);
        engine.add(FXForward(1.10, 10.0));
        CrossCurrencyScenarios scenarios;
        const std::size_t N = 40000;
        const ExposurePaths cube = engine.simulate(settings(N, false), nullptr, &scenarios);
        const std::size_t n = cube.dates();
        const Real ad = 0.03, af = 0.05, sd = 0.010, sf = 0.008, rho = 0.35;
        for (const Time t : {1.0, 3.0, 6.0, 9.0})
        {
            const std::size_t i = static_cast<std::size_t>(
                std::lower_bound(cube.times.begin(), cube.times.end(), t - 1e-9) - cube.times.begin());
            ASSERT_NEAR(cube.times[i], t, 1e-9);
            Real md = 0.0, mf = 0.0, ml = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                md += scenarios.domestic[p * n + i] / N;
                mf += scenarios.foreign[p * n + i] / N;
                ml += std::log(scenarios.spot[p * n + i]) / N;
            }
            Real covariance = 0.0, variance = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                covariance += (scenarios.domestic[p * n + i] - md) * (scenarios.foreign[p * n + i] - mf) / N;
                const Real l = std::log(scenarios.spot[p * n + i]) - ml;
                variance += l * l / N;
            }
            // The two rates: ρ σ_d σ_f (1 - e^{-(a_d + a_f) t}) / (a_d + a_f).
            const Real exact = rho * sd * sf * -std::expm1(-(ad + af) * t) / (ad + af);
            const Real var_d = sd * sd * -std::expm1(-2.0 * ad * t) / (2.0 * ad);
            const Real var_f = sf * sf * -std::expm1(-2.0 * af * t) / (2.0 * af);
            EXPECT_NEAR(covariance, exact, 4.0 * std::sqrt((var_d * var_f + exact * exact) / N)) << t;
            // The spot: the Black variance of an option expiring at t. It is
            // read back from the three factors, so every covariance is in it.
            const Real implied = m.fx_implied_volatility(t);
            EXPECT_NEAR(variance, implied * implied * t, 4.0 * implied * implied * t * std::sqrt(2.0 / N))
                << t;
        }
    }

    TEST(CrossCurrencyEngine, ACrossCurrencySwapIsItsCashFlowsExchangedOneByOne)
    {
        const CrossCurrencyHullWhiteModel m = model();
        const Time T = 5.0;
        const CrossCurrencySwap ccs = par_ccs(m, T);
        CrossCurrencyExposureEngine engine(m);
        engine.add(ccs);
        // The same flows as forwards: each euro coupon against its dollar
        // coupon, and the notionals at maturity.
        const Real rate = ccs.domestic_notional * ccs.domestic_rate / (ccs.foreign_notional * ccs.foreign_rate);
        for (int k = 1; k <= 5; ++k)
            engine.add(FXForward(rate, static_cast<Real>(k), ccs.foreign_notional * ccs.foreign_rate));
        engine.add(FXForward(ccs.domestic_notional / ccs.foreign_notional, T, ccs.foreign_notional));
        const ExposurePaths cube = engine.simulate(settings(300));

        // Both legs at par, notionals at the spot: worth nothing today.
        EXPECT_NEAR(cube.trade_values_today[0], 0.0, 1e-10);
        for (std::size_t j = 0; j < cube.trade_values[0].size(); ++j)
        {
            Real forwards = 0.0, flows = 0.0;
            for (std::size_t k = 1; k < cube.trades(); ++k)
            {
                forwards += cube.trade_values[k][j];
                flows += cube.trade_cashflows[k][j];
            }
            ASSERT_NEAR(cube.trade_values[0][j], forwards, 1e-10);
            ASSERT_NEAR(cube.trade_cashflows[0][j], flows, 1e-10);
        }
        // Seen from the other side: the opposite values.
        CrossCurrencyExposureEngine other(m);
        CrossCurrencySwap paying = ccs;
        paying.receive_foreign = false;
        other.add(paying);
        const ExposurePaths mirror = other.simulate(settings(300));
        for (std::size_t j = 0; j < cube.trade_values[0].size(); ++j)
            ASSERT_DOUBLE_EQ(mirror.trade_values[0][j], -cube.trade_values[0][j]);
    }

    TEST(CrossCurrencyEngine, TheExposureOfACrossCurrencySwapIsThatOfItsFinalExchange)
    {
        const CrossCurrencyHullWhiteModel m = model();
        const Time T = 10.0;
        CrossCurrencyExposureEngine engine(m);
        engine.add(par_ccs(m, T));
        // The final exchange alone, at today's spot.
        engine.add(FXForward(m.spot(), T, 100.0));
        // A swap of one currency, for the shape it does not have.
        engine.add_domestic(swap(m.domestic(), T, 0.0, true, 100.0 * m.spot()));
        const ExposurePaths cube = engine.simulate(settings(8000, false));
        const ExposureStatistics ccs = exposure_statistics(cube, {0});
        const ExposureStatistics exchange = exposure_statistics(cube, {1});
        const ExposureStatistics rate_swap = exposure_statistics(cube, {2});

        const auto peak = [](const ExposureStatistics &s)
        {
            return s.times[static_cast<std::size_t>(std::max_element(s.ee.begin(), s.ee.end()) -
                                                    s.ee.begin())];
        };
        // A swap's exposure is a hump that peaks in the first half of its
        // life and is back at zero at maturity; the cross-currency swap's
        // keeps rising and peaks just before the notionals come back.
        EXPECT_LT(peak(rate_swap), 0.5 * T);
        EXPECT_GT(peak(ccs), 0.85 * T);
        // Year after year, higher.
        const auto at = [&ccs](Time t)
        {
            const auto it = std::lower_bound(ccs.times.begin(), ccs.times.end(), t - 1e-9);
            return ccs.ee[static_cast<std::size_t>(it - ccs.times.begin())];
        };
        for (int year = 2; year <= 9; ++year)
            EXPECT_GT(at(year + 0.5), at(year - 0.5));
        // Several times the exposure of a rate swap of the same notional.
        const Real ccs_peak = *std::max_element(ccs.ee.begin(), ccs.ee.end());
        EXPECT_GT(ccs_peak, 3.0 * *std::max_element(rate_swap.ee.begin(), rate_swap.ee.end()));
        // And late in its life it is the final exchange: the coupons left
        // are small next to the notional.
        const std::size_t late = static_cast<std::size_t>(
            std::lower_bound(ccs.times.begin(), ccs.times.end(), 9.5 - 1e-9) - ccs.times.begin());
        EXPECT_NEAR(ccs.ee[late] / exchange.ee[late], 1.0, 0.15);
    }

    TEST(CrossCurrencyEngine, TheRestOfTheChainAppliesUnchanged)
    {
        const CrossCurrencyHullWhiteModel m = model();
        CrossCurrencyExposureEngine engine(m);
        engine.add(par_ccs(m, 5.0));
        engine.add_domestic(swap(m.domestic(), 5.0, 0.0, true));
        engine.add_foreign(swap(m.foreign(), 5.0, 0.0, false));
        ExposureSimulationSettings s = settings(4000);
        s.grid.margin_period_of_risk = 10.0 / 250.0;
        const ExposurePaths cube = engine.simulate(s);

        XvaInputs in;
        in.counterparty = CreditCurve(0.02);
        in.own = CreditCurve(0.01);
        in.borrowing_spread = in.lending_spread = 0.005;
        const XvaReport open = xva_report(cube, in);
        EXPECT_LT(open.cva.value, 0.0);
        EXPECT_GT(open.dva.value, 0.0);
        EXPECT_GT(open.cva.error, 0.0);
        ASSERT_EQ(open.contributions.size(), 3u);
        // The Euler shares add up to the CVA of the set.
        Real shares = 0.0;
        for (const TradeContribution &c : open.contributions)
            shares += c.marginal_cva;
        EXPECT_NEAR(shares, open.cva.value, 1e-9 * std::abs(open.cva.value));
        // The cross-currency swap carries most of it.
        EXPECT_LT(open.contributions[0].standalone_cva, 3.0 * open.contributions[1].standalone_cva);
        // Under a CSA the exposure is what moves in ten days.
        in.csa = Csa{};
        in.csa->margin_period_of_risk = 10.0 / 250.0;
        const XvaReport margined = xva_report(cube, in);
        EXPECT_GT(margined.cva.value, 0.2 * open.cva.value);
        EXPECT_LT(margined.cva.value, 0.0);
    }

    TEST(CrossCurrencyEngine, TheSameBitsOnOneThreadOrMany)
    {
        const CrossCurrencyHullWhiteModel m = model();
        CrossCurrencyExposureEngine engine(m);
        engine.add(par_ccs(m, 3.0));
        engine.add_foreign(swap(m.foreign(), 3.0, 0.0, true));
        const ExposurePaths one = engine.simulate(settings(1000));
        ThreadPool pool;
        pool.start(4);
        const ExposurePaths many = engine.simulate(settings(1000), &pool);
        EXPECT_EQ(one.trade_values, many.trade_values);
        EXPECT_EQ(one.trade_cashflows, many.trade_cashflows);
        EXPECT_EQ(one.discount_weight, many.discount_weight);
        // Another seed, other scenarios.
        ExposureSimulationSettings other = settings(1000);
        other.seed = 7;
        EXPECT_NE(engine.simulate(other).trade_values, one.trade_values);
    }

    TEST(CrossCurrencyEngine, RefusesWhatItDoesNotDo)
    {
        const CrossCurrencyHullWhiteModel m = model();
        CrossCurrencyExposureEngine engine(m);
        EXPECT_THROW(engine.simulate(), InvalidInput); // no trade
        // A rate trade says which currency it is in.
        EXPECT_THROW(engine.add(swap(m.domestic(), 5.0, 0.0, true)), UnsupportedInstrument);
        // A Bermudan is valued by regression.
        const InterestRateSwap underlying = make_swap(1.0, 4.0, 0.04, 1, 4, 100.0, true);
        EXPECT_THROW(engine.add_domestic(BermudanSwaption(underlying, {1.0, 2.0, 3.0})),
                     UnsupportedInstrument);
        EXPECT_THROW(engine.add(CrossCurrencySwap(2.5, 100.0, 110.0, 0.02, 0.04)), InvalidInput);
        EXPECT_THROW(engine.add(CrossCurrencySwap(2.0, 0.0, 110.0, 0.02, 0.04)), InvalidInput);
        EXPECT_THROW(engine.add(FXForward(0.0, 1.0)), InvalidInput);

        engine.add(FXForward(1.1, 1.0));
        ExposureSimulationSettings s = settings(100);
        s.device = ComputeDevice::Gpu;
        EXPECT_THROW(engine.simulate(s), InvalidInput);
        s.device = ComputeDevice::Auto;
        EXPECT_EQ(engine.simulate(s).device, "cpu");
        EXPECT_FALSE(engine.simulate(s).device_note.empty());
        s = settings(100);
        s.simm = true;
        EXPECT_THROW(engine.simulate(s), InvalidInput);
        s = settings(100);
        s.historical = HistoricalRateDynamics{0.1, 0.03, 0.01};
        EXPECT_THROW(engine.simulate(s), InvalidInput);
        s = settings(0);
        EXPECT_THROW(engine.simulate(s), InvalidInput);
        s = settings(100);
        s.memory_limit_bytes = 1024;
        EXPECT_THROW(engine.simulate(s), InvalidInput);
    }

} // namespace quantModeling
