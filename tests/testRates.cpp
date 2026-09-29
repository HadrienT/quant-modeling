#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace quantModeling
{
    namespace
    {
        /// An upward-sloping OIS curve bootstrapped from par OIS rates.
        DiscountCurve ois_curve()
        {
            std::vector<ParRateQuote> q;
            for (const auto &[T, r] : std::vector<std::pair<Time, Real>>{
                     {1.0, 0.0400}, {2.0, 0.0385}, {3.0, 0.0378}, {5.0, 0.0375}, {7.0, 0.0380}, {10.0, 0.0390}, {15.0, 0.0400}, {20.0, 0.0402}, {30.0, 0.0395}})
                q.push_back(make_ois_quote(T, r));
            return bootstrap_curve({{0.25, 0.0410}, {0.5, 0.0405}}, q);
        }

        std::vector<FraQuote> three_month_fras()
        {
            return {{0.0, 0.25, 0.0440, 0.25}, {0.25, 0.5, 0.0432, 0.25}, {0.5, 0.75, 0.0425, 0.25}, {0.75, 1.0, 0.0420, 0.25}};
        }

        std::vector<ProjectionSwapQuote> three_month_swaps()
        {
            std::vector<ProjectionSwapQuote> q;
            for (const auto &[T, r] : std::vector<std::pair<Time, Real>>{
                     {2.0, 0.0412}, {3.0, 0.0405}, {5.0, 0.0402}, {7.0, 0.0406}, {10.0, 0.0415}, {15.0, 0.0424}, {20.0, 0.0426}, {30.0, 0.0420}})
                q.push_back(make_projection_swap_quote(T, r));
            return q;
        }

        DiscountCurve projection_curve(const DiscountCurve &ois)
        {
            return bootstrap_projection_curve(ois, three_month_fras(), three_month_swaps());
        }

        /// Hull-White ZCB put, Brigo & Mercurio (2006) eq. 3.41 — the
        /// textbook formula, independent of the region integration.
        Real hw_zcb_put(const HullWhiteCurveModel &m, Time T, Time S, Real X)
        {
            const Real a = m.mean_reversion(), s = m.sigma();
            const Real sp = s * std::sqrt((1.0 - std::exp(-2.0 * a * T)) / (2.0 * a)) * (1.0 - std::exp(-a * (S - T))) / a;
            const Real PT = m.discount().discount(T), PS = m.discount().discount(S);
            const Real h = std::log(PS / (PT * X)) / sp + 0.5 * sp;
            return X * PT * norm_cdf(-h + sp) - PS * norm_cdf(-h);
        }
    } // namespace

    // ── Curves ──────────────────────────────────────────────────────────────

    TEST(MultiCurve, OisBootstrapRepricesEveryParOisSwapAtPar)
    {
        const DiscountCurve ois = ois_curve();
        for (const auto &[T, r] : std::vector<std::pair<Time, Real>>{{2.0, 0.0385}, {7.0, 0.0380}, {30.0, 0.0395}})
        {
            // An OIS is a swap whose floating index is the discount curve.
            InterestRateSwap s = make_swap(0.0, T, r, 1, 1);
            EXPECT_NEAR(value_swap(s, MultiCurve{ois, ois}).npv, 0.0, 1e-10) << "T=" << T;
        }
    }

    TEST(MultiCurve, ProjectionCurveRepricesEveryFraAndSwap)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        for (const FraQuote &f : three_month_fras())
            EXPECT_NEAR(forward_rate(proj, f.start, f.end, f.accrual), f.rate, 1e-12);
        for (const ProjectionSwapQuote &q : three_month_swaps())
            EXPECT_NEAR(value_swap(q.swap, MultiCurve{ois, proj}).par_rate, q.swap.fixed_rate, 1e-12)
                << "T=" << q.maturity();
    }

    TEST(MultiCurve, QuotesImpliedByTheDiscountCurveGiveBackTheSingleCurve)
    {
        // Single-curve limit: swaps quoted at the OIS curve's own par rates
        // bootstrap a projection curve with the OIS forwards.
        const DiscountCurve ois = ois_curve();
        // Same pillars as the OIS curve, so the two interpolate alike.
        std::vector<FraQuote> fras{{0.0, 0.25, forward_rate(ois, 0.0, 0.25, 0.25), 0.25},
                                   {0.25, 0.5, forward_rate(ois, 0.25, 0.5, 0.25), 0.25}};
        std::vector<ProjectionSwapQuote> swaps;
        for (const Time T : {1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0})
        {
            ProjectionSwapQuote q = make_projection_swap_quote(T, 0.0);
            q.swap.fixed_rate = value_swap(q.swap, MultiCurve{ois, ois}).par_rate;
            swaps.push_back(q);
        }
        const DiscountCurve proj = bootstrap_projection_curve(ois, fras, swaps);
        for (Time t = 0.25; t <= 30.0; t += 0.25)
            EXPECT_NEAR(proj.discount(t), ois.discount(t), 1e-12) << "t=" << t;
    }

    TEST(MultiCurve, ThreeMonthForwardsSitAboveOis)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        for (Time s = 0.0; s < 29.5; s += 0.25)
            EXPECT_GT(forward_rate(proj, s, s + 0.25, 0.25), forward_rate(ois, s, s + 0.25, 0.25)) << "s=" << s;
    }

    // ── Swaps ───────────────────────────────────────────────────────────────

    TEST(Swap, SingleCurveFloatingLegTelescopesToOneMinusTheLastDiscountFactor)
    {
        const DiscountCurve ois = ois_curve();
        const InterestRateSwap s = make_swap(0.0, 10.0, 0.0, 1, 4);
        EXPECT_NEAR(value_swap(s, MultiCurve{ois, ois}).floating_leg, 1.0 - ois.discount(10.0), 1e-14);
    }

    TEST(Swap, PayerAndReceiverCancelAndTheParRateIsWorthZero)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        InterestRateSwap payer = make_swap(1.0, 5.0, 0.045, 1, 4, 1e6, true, 0.001);
        InterestRateSwap receiver = payer;
        receiver.payer = false;
        const MultiCurve mc{ois, proj};
        EXPECT_NEAR(value_swap(payer, mc).npv + value_swap(receiver, mc).npv, 0.0, 1e-8);
        payer.fixed_rate = value_swap(payer, mc).par_rate;
        EXPECT_NEAR(value_swap(payer, mc).npv, 0.0, 1e-8);
    }

    // ── Swaption formulas ───────────────────────────────────────────────────

    TEST(SwaptionFormulas, PayerMinusReceiverIsTheForwardSwap)
    {
        const Real F = 0.041, A = 4.3, T = 2.0;
        for (const Real K : {0.02, 0.041, 0.07})
        {
            EXPECT_NEAR(bachelier_swaption(true, F, K, T, 0.009, A) - bachelier_swaption(false, F, K, T, 0.009, A),
                        A * (F - K), 1e-14);
            EXPECT_NEAR(black_swaption(true, F, K, T, 0.25, A, 0.01) - black_swaption(false, F, K, T, 0.25, A, 0.01),
                        A * (F - K), 1e-14);
        }
    }

    TEST(SwaptionFormulas, BachelierImpliedVolRoundTrips)
    {
        const Real F = 0.035, A = 7.1;
        for (const Real T : {0.25, 1.0, 10.0})
            for (const Real K : {-0.005, 0.02, 0.035, 0.06})
                for (const Real vol : {0.002, 0.009, 0.02})
                    for (const bool payer : {true, false})
                    {
                        if (std::abs(F - K) > 8.0 * vol * std::sqrt(T))
                            continue; // beyond 8 sd the time value underflows its own formula
                        const Real p = bachelier_swaption(payer, F, K, T, vol, A);
                        // Deep in or out of the money the time value is a
                        // few ulps of the price: no inversion can do better.
                        if (p - A * std::max((payer ? 1 : -1) * (F - K), 0.0) < 1e-6 * p)
                            continue;
                        EXPECT_NEAR(bachelier_implied_vol(payer, p, F, K, T, A), vol, 1e-9)
                            << "T=" << T << " K=" << K << " vol=" << vol;
                    }
        EXPECT_TRUE(std::isnan(bachelier_implied_vol(true, 0.0, F, 0.02, 1.0, A))); // below intrinsic
    }

    TEST(SwaptionFormulas, BlackAtTheMoneyMatchesBachelierWithNormalVolSigmaTimesForward)
    {
        // To first order in σ√T, the ATM Black and normal prices agree when
        // σ_N = σ_B F.
        const Real F = 0.04, A = 5.0, T = 0.5, sb = 0.05;
        EXPECT_NEAR(black_swaption(true, F, F, T, sb, A) / bachelier_swaption(true, F, F, T, sb * F, A), 1.0, 1e-4);
    }

    TEST(SwaptionFormulas, ShiftedSabrWithoutVolOfVolIsShiftedBlackAtAlphaOverFBeta)
    {
        const Real F = 0.004, K = 0.001, T = 3.0, A = 2.0, shift = 0.02;
        const SABRParams p{0.03, 0.5, 0.0, 1e-8};
        const Real vol = sabr_implied_vol(F + shift, K + shift, T, p);
        EXPECT_NEAR(sabr_swaption(true, F, K, T, p, A, shift), black_swaption(true, F, K, T, vol, A, shift), 1e-15);
    }

    // ── Hull-White on the curve ─────────────────────────────────────────────

    TEST(HullWhiteCurve, ReproducesTheCurveAndIsAMartingaleUnderEachForwardMeasure)
    {
        const DiscountCurve ois = ois_curve();
        const HullWhiteCurveModel m(0.05, 0.011, ois);
        for (const Time T : {0.5, 3.0, 12.0})
            EXPECT_NEAR(m.zcb(0.0, T, 0.0), ois.discount(T), 1e-15);

        // E^T[P(t, S) / P(t, T)] = P(0, S) / P(0, T), by Gauss-Hermite-like
        // quadrature on the exact Gaussian transition from 0 to t.
        const Time t = 4.0, T = 5.0, S = 9.0;
        const auto tr = m.transition(0.0, t, T);
        const int n = 4000;
        Real sum = 0.0, wsum = 0.0;
        for (int i = 0; i <= n; ++i)
        {
            const Real z = -10.0 + 20.0 * i / n;
            const Real w = norm_pdf(z) * ((i == 0 || i == n) ? 0.5 : 1.0);
            const Real x = tr.drift + std::sqrt(tr.variance) * z;
            sum += w * m.zcb(t, S, x) / m.zcb(t, T, x);
            wsum += w;
        }
        EXPECT_NEAR(sum / wsum, ois.discount(S) / ois.discount(T), 1e-10);
        // Under the t-forward measure x(t) is centred.
        EXPECT_NEAR(m.transition(0.0, t, t).drift, 0.0, 1e-14);
    }

    TEST(HullWhiteCurve, EuropeanSwaptionMatchesJamshidiansZeroCouponBondPuts)
    {
        const DiscountCurve ois = ois_curve();
        const HullWhiteCurveModel m(0.04, 0.012, ois);
        const Time T = 3.0;
        for (const Real K : {0.03, 0.038, 0.05})
        {
            // Single-curve payer = put on the coupon bond Σ c_i P(T, t_i), strike 1.
            Swaption swpt{make_swap(T, 7.0, K, 1, 4), T};
            std::vector<std::pair<Time, Real>> bond;
            for (const CouponPeriod &c : swpt.swap.fixed_leg)
                bond.push_back({c.payment, K * c.accrual});
            bond.back().second += 1.0;
            const auto cb = [&](Real x)
            {
                Real v = 0.0;
                for (const auto &[t, c] : bond)
                    v += c * m.zcb(T, t, x);
                return v;
            };
            Real lo = -1.0, hi = 1.0; // cb is decreasing in x
            for (int i = 0; i < 200; ++i)
                (cb(0.5 * (lo + hi)) > 1.0 ? lo : hi) = 0.5 * (lo + hi);
            const Real xs = 0.5 * (lo + hi);
            Real jamshidian = 0.0;
            for (const auto &[t, c] : bond)
                jamshidian += c * hw_zcb_put(m, T, t, m.zcb(T, t, xs));
            EXPECT_NEAR(hull_white_european_swaption(swpt, m), jamshidian, 1e-12) << "K=" << K;
        }
    }

    TEST(HullWhiteCurve, MultiCurveEuropeanSwaptionMatchesMonteCarlo)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        const HullWhiteCurveModel m(0.03, 0.01, ois, proj);
        const Time T = 5.0;
        Swaption swpt{make_swap(T, 10.0, 0.043, 1, 4, 1.0, false), T};
        const Real closed = hull_white_european_swaption(swpt, m);

        // Under the T-forward measure x(T) ~ N(0, y(T)): sample it, value the
        // swap's bond portfolio, average the positive part.
        const std::vector<BondCashflow> cfs = swap_as_bonds(swpt.swap, T, m);
        std::mt19937_64 gen(7);
        std::normal_distribution<Real> z;
        const int n = 100000;
        Real sum = 0.0, sum2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const Real x = std::sqrt(m.y(T)) * z(gen);
            Real v = 0.0;
            for (const BondCashflow &c : cfs)
                v += c.amount * m.zcb(T, c.time, x);
            const Real payoff = ois.discount(T) * std::max(v, 0.0);
            sum += payoff;
            sum2 += payoff * payoff;
        }
        const Real mean = sum / n, se = std::sqrt((sum2 / n - mean * mean) / n);
        EXPECT_NEAR(closed, mean, 4.0 * se);
    }

    TEST(HullWhiteCurve, BermudanWithOneExerciseIsTheEuropean)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        const HullWhiteCurveModel m(0.03, 0.01, ois, proj);
        const InterestRateSwap swap = make_swap(2.0, 8.0, 0.042, 1, 4);
        const Real european = hull_white_european_swaption({swap, 2.0}, m);
        const Real bermudan = hull_white_bermudan_swaption({swap, {2.0}}, m);
        EXPECT_NEAR(bermudan / european, 1.0, 1.5e-4); // O(h²) of the grid, see the next test
    }

    TEST(HullWhiteCurve, BermudanIsWorthAtLeastEveryCoTerminalEuropeanAndMoreWithMoreDates)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        const HullWhiteCurveModel m(0.03, 0.01, ois, proj);
        const InterestRateSwap swap = make_swap(1.0, 9.0, 0.042, 1, 4, 1.0, false);
        std::vector<Time> annual;
        for (int i = 1; i <= 9; ++i)
            annual.push_back(i);
        const Real berm = hull_white_bermudan_swaption({swap, annual}, m);
        Real best_european = 0.0;
        for (const Time t : annual)
            best_european = std::max(best_european, hull_white_european_swaption({swap.tail_from(t), t}, m));
        EXPECT_GT(berm, best_european);
        const Real fewer = hull_white_bermudan_swaption({swap, {1.0, 3.0, 5.0, 7.0}}, m);
        EXPECT_GT(berm, fewer);
        EXPECT_GT(fewer, best_european * (1.0 - 1.5e-4));
    }

    TEST(HullWhiteCurve, BermudanLatticeConvergesAtSecondOrderInTheGridStep)
    {
        const DiscountCurve ois = ois_curve();
        const HullWhiteCurveModel m(0.05, 0.012, ois);
        const BermudanSwaption b{make_swap(1.0, 10.0, 0.04, 1, 4), {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0}};
        const auto at = [&](int n)
        { return hull_white_bermudan_swaption(b, m, {n, 8.0}); };
        const Real v1 = at(101), v2 = at(201), v3 = at(401);
        const Real ratio = (v1 - v2) / (v2 - v3);
        EXPECT_GT(ratio, 3.0);
        EXPECT_LT(ratio, 5.5);
    }

    // ── Calibration ─────────────────────────────────────────────────────────

    TEST(HullWhiteCalibration, RecoversTheParametersThatGeneratedTheVols)
    {
        const DiscountCurve ois = ois_curve();
        const DiscountCurve proj = projection_curve(ois);
        const HullWhiteCurveModel truth(0.045, 0.0105, ois, proj);
        std::vector<SwaptionVolQuote> quotes;
        for (const Time e : {1.0, 2.0, 5.0, 10.0})
            for (const Time t : {2.0, 5.0, 10.0})
                quotes.push_back({e, t, hull_white_atm_normal_vol(truth, e, t)});
        const HullWhiteCalibration c = calibrate_hull_white(ois, proj, quotes);
        EXPECT_TRUE(c.report.converged);
        EXPECT_NEAR(c.mean_reversion, 0.045, 1e-6);
        EXPECT_NEAR(c.sigma, 0.0105, 1e-8);
        EXPECT_LT(c.report.rmse, 1e-4); // bp
    }

    TEST(HullWhiteCalibration, WithAFixedMeanReversionOnlySigmaMoves)
    {
        const DiscountCurve ois = ois_curve();
        const HullWhiteCurveModel truth(0.02, 0.009, ois);
        std::vector<SwaptionVolQuote> quotes;
        for (const Time e : {1.0, 3.0, 5.0})
            quotes.push_back({e, 10.0 - e, hull_white_atm_normal_vol(truth, e, 10.0 - e)});
        const HullWhiteCalibration c = calibrate_hull_white(ois, ois, quotes, 1, 4, 0.02);
        EXPECT_DOUBLE_EQ(c.mean_reversion, 0.02);
        EXPECT_NEAR(c.sigma, 0.009, 1e-8);
    }

} // namespace quantModeling
