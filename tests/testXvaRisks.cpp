#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/xva.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// Lot X8 of blueprint/wp/23-xva.md (§14.13): the sensitivities of CVA, DVA
// and FVA by adjoint differentiation. The oracle is the lot's criterion: the
// same estimator in doubles, bumped, on the same scenarios.

namespace quantModeling
{
    namespace
    {
        const std::vector<Time> kPillars{1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0};

        /// Everything a run depends on, so that any of it can be bumped.
        struct Market
        {
            std::vector<Real> zero{0.031, 0.033, 0.034, 0.036, 0.037, 0.038, 0.039};
            Real a = 0.04, sigma = 0.011;
            std::vector<Time> hazard_times{2.0, 5.0, 10.0};
            std::vector<Real> hazard_c{0.012, 0.018, 0.025};
            Real hazard_i = 0.008;
            Real lgd_c = 0.6, lgd_i = 0.55, spread_b = 0.004, spread_l = 0.0015;
        };

        /// The inputs in the order xva_risks() reports them.
        std::vector<Real *> factors(Market &m)
        {
            std::vector<Real *> out;
            for (Real &z : m.zero)
                out.push_back(&z);
            out.push_back(&m.a);
            out.push_back(&m.sigma);
            for (Real &h : m.hazard_c)
                out.push_back(&h);
            out.push_back(&m.hazard_i);
            for (Real *x : {&m.lgd_c, &m.lgd_i, &m.spread_b, &m.spread_l})
                out.push_back(x);
            return out;
        }

        DiscountCurve curve(const Market &m)
        {
            std::vector<Real> dfs;
            for (std::size_t k = 0; k < kPillars.size(); ++k)
                dfs.push_back(std::exp(-m.zero[k] * kPillars[k]));
            return DiscountCurve(kPillars, dfs, CurveExtrapolation::FlatForward);
        }

        using Book = std::function<void(HullWhiteExposureEngine &)>;

        /// Swaps on both sides, one of them forward starting. Fixed rates are
        /// numbers, not par rates: a bumped curve leaves the trades alone.
        void swaps(HullWhiteExposureEngine &engine)
        {
            engine.add(make_swap(0.0, 10.0, 0.038, 1, 2, 1e7, true));
            engine.add(make_swap(0.0, 5.0, 0.031, 1, 2, 6e6, false));
            engine.add(make_swap(1.0, 6.0, 0.040, 1, 1, 4e6, true), 1.5);
        }

        /// Options bought and sold, with a swap.
        void options(HullWhiteExposureEngine &engine)
        {
            engine.add(Swaption{make_swap(2.0, 5.0, 0.039, 1, 2, 1e7, true), 2.0});
            engine.add(Swaption{make_swap(1.0, 4.0, 0.036, 1, 1, 6e6, false), 1.0}, -1.0);
            engine.add(make_swap(0.0, 7.0, 0.036, 1, 2, 5e6, true));
        }

        XvaRiskInputs inputs(const Market &m, bool csa)
        {
            XvaRiskInputs in;
            in.counterparty = CreditCurve(m.hazard_times, m.hazard_c);
            in.own = CreditCurve(m.hazard_i);
            in.lgd_counterparty = m.lgd_c;
            in.lgd_own = m.lgd_i;
            in.borrowing_spread = m.spread_b;
            in.lending_spread = m.spread_l;
            if (csa)
            {
                Csa terms;
                terms.threshold_counterparty = 5e4;
                terms.independent_amount = 1e4;
                in.csa = terms;
                in.cashflows = MarginPeriodCashflows::Withheld;
            }
            return in;
        }

        ExposureSimulationSettings settings(std::size_t paths)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 20261002;
            return s;
        }

        /// A run on a market: the model and the engine are rebuilt, as a
        /// bump-and-reprice would.
        template <class Result, class Run>
        Result on(const Market &m, const Book &book, const Run &run)
        {
            const HullWhiteCurveModel model(m.a, m.sigma, curve(m));
            HullWhiteExposureEngine engine(model);
            book(engine);
            return run(engine);
        }

        XvaRisks risks(const Market &m, const Book &book, bool csa, std::size_t paths, Real smoothing = 0.05,
                       ThreadPool *pool = nullptr)
        {
            return on<XvaRisks>(m, book,
                                [&](HullWhiteExposureEngine &engine)
                                {
                                    XvaRiskInputs in = inputs(m, csa);
                                    in.exercise_smoothing = smoothing;
                                    return engine.xva_risks(settings(paths), in, pool);
                                });
        }

        XvaValues values(const Market &m, const Book &book, bool csa, std::size_t paths,
                         const std::vector<Real> &widths)
        {
            return on<XvaValues>(m, book,
                                 [&](HullWhiteExposureEngine &engine)
                                 {
                                     XvaRiskInputs in = inputs(m, csa);
                                     in.smoothing_widths = widths;
                                     return engine.xva_values(settings(paths), in);
                                 });
        }

        /// Every adjoint sensitivity against a central difference of the same
        /// estimator on the same scenarios.
        void expect_adjoint_is_the_bump(const Book &book, bool csa, std::size_t paths)
        {
            const Market base;
            const XvaRisks adjoint = risks(base, book, csa, paths);
            ASSERT_EQ(adjoint.factors.size(), 17u);
            std::size_t compared = 0;
            for (std::size_t j = 0; j < adjoint.factors.size(); ++j)
            {
                Market up = base, down = base;
                const Real level = *factors(up)[j];
                EXPECT_DOUBLE_EQ(level, adjoint.levels[j]) << adjoint.labels[j];
                // A small bump: the estimator has a kink wherever a path's
                // exposure changes sign, and a path that crosses zero inside
                // the bump puts the difference off by its own share.
                const Real h = 2e-7 * std::max(std::abs(level), 0.01);
                *factors(up)[j] += h;
                *factors(down)[j] -= h;
                const XvaValues high = values(up, book, csa, paths, adjoint.smoothing_widths);
                const XvaValues low = values(down, book, csa, paths, adjoint.smoothing_widths);
                for (std::size_t o = 0; o < kXvaOutputs; ++o)
                {
                    const Real bump = (high.values[o].value - low.values[o].value) / (2.0 * h);
                    const Real risk = adjoint.risks[o * adjoint.factors.size() + j];
                    // Both differentiate the same function of the same draws:
                    // they agree to the rounding of the difference, orders of
                    // magnitude inside the Monte-Carlo error of either.
                    EXPECT_NEAR(risk, bump, 2e-5 * std::abs(bump) + 1e-5 * std::abs(adjoint.values[o].value))
                        << adjoint.labels[j] << ", output " << o;
                    if (std::abs(bump) > 0.0)
                        ++compared;
                }
            }
            // Not a comparison of zeros.
            EXPECT_GT(compared, 50u);
        }
    } // namespace

    TEST(XvaRisks, WithoutSmoothingTheValuesAreThePricingEngines)
    {
        const Market m;
        for (const bool csa : {false, true})
        {
            const XvaRiskInputs in = inputs(m, csa);
            const HullWhiteCurveModel model(m.a, m.sigma, curve(m));
            HullWhiteExposureEngine engine(model);
            options(engine);
            XvaRiskInputs sharp = in;
            sharp.exercise_smoothing = 0.0;
            const XvaValues direct = engine.xva_values(settings(3000), sharp);

            ExposureSimulationSettings s = settings(3000);
            s.keep_cashflows = true;
            if (csa)
                s.grid.margin_period_of_risk = in.csa->margin_period_of_risk;
            const ExposurePaths cube = engine.simulate(s);
            CollateralSettings collateral;
            collateral.cashflows = in.cashflows;
            ExposureProfile profile = csa ? exposure_profile(collateralise(cube, *in.csa, {}, collateral))
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
            for (const Real w : direct.smoothing_widths)
                EXPECT_EQ(w, 0.0);
        }
    }

    TEST(XvaRisks, TheAdjointRunGivesTheValuesOfTheDoubleRun)
    {
        const Market m;
        const XvaRisks adjoint = risks(m, options, true, 1000);
        const XvaValues plain = values(m, options, true, 1000, adjoint.smoothing_widths);
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
        {
            EXPECT_NEAR(adjoint.values[o].value, plain.values[o].value, 1e-10 * std::abs(plain.values[o].value));
            EXPECT_NEAR(adjoint.values[o].error, plain.values[o].error, 1e-8 * plain.values[o].error);
        }
        // The two options are smoothed, the swap is not.
        ASSERT_EQ(adjoint.smoothing_widths.size(), 3u);
        EXPECT_GT(adjoint.smoothing_widths[0], 0.0);
        EXPECT_GT(adjoint.smoothing_widths[1], 0.0);
        EXPECT_EQ(adjoint.smoothing_widths[2], 0.0);
        EXPECT_EQ(adjoint.batches, 16u); // 1000 paths: fifteen batches of 64 and one of 40
        EXPECT_EQ(adjoint.batch_risks.size(), 16u * kXvaOutputs * adjoint.factors.size());
    }

    TEST(XvaRisks, OnSwapsEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_the_bump(swaps, false, 128);
    }

    TEST(XvaRisks, UnderACsaEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_the_bump(swaps, true, 128);
    }

    TEST(XvaRisks, OnOptionsEveryAdjointSensitivityIsTheBump)
    {
        expect_adjoint_is_the_bump(options, false, 128);
        expect_adjoint_is_the_bump(options, true, 128);
    }

    TEST(XvaRisks, WithoutSmoothingTheAdjointOfAnOptionMissesTheExerciseBoundary)
    {
        // A swaption bought, physically settled: after its expiry it is a
        // swap on the paths that exercised and nothing on the others. Path by
        // path the indicator does not move with the curve, so its adjoint
        // misses the paths that change sides; the ramp gives them back.
        const Market base;
        const Book bought = [](HullWhiteExposureEngine &engine)
        { engine.add(Swaption{make_swap(2.0, 8.0, 0.039, 1, 2, 1e7, true), 2.0}); };
        const std::size_t paths = 64 * 250, ten_years = 5;
        ThreadPool pool;
        pool.start(7);

        // The reference: the pricing engine's own CVA (no ramp), bumped
        // widely enough to average over the paths that cross.
        const auto cva = [&](const Market &m)
        {
            return on<Real>(m, bought,
                            [&](HullWhiteExposureEngine &engine)
                            {
                                XvaRiskInputs in = inputs(m, false);
                                in.smoothing_widths = {0.0};
                                return engine.xva_values(settings(paths), in, &pool)[XvaOutput::Cva].value;
                            });
        };
        Market up = base, down = base;
        const Real h = 0.02 * base.zero[ten_years];
        up.zero[ten_years] += h;
        down.zero[ten_years] -= h;
        const Real bump = (cva(up) - cva(down)) / (2.0 * h);

        const XvaRisks sharp = risks(base, bought, false, paths, 0.0, &pool);
        const XvaRisks smooth = risks(base, bought, false, paths, 0.05, &pool);
        pool.stop();
        EXPECT_EQ(sharp.labels[ten_years], "zero rate 10Y");
        EXPECT_LT(bump, 0.0);
        // More than a tenth of the sensitivity is at the boundary...
        EXPECT_GT(std::abs(sharp.risks[ten_years] - bump), 0.10 * std::abs(bump));
        // ... and the ramp recovers it, at no cost on the value.
        EXPECT_NEAR(smooth.risks[ten_years], bump, 0.03 * std::abs(bump));
        EXPECT_NEAR(smooth[XvaOutput::Cva].value, sharp[XvaOutput::Cva].value,
                    0.2 * sharp[XvaOutput::Cva].error);
    }

    TEST(XvaRisks, TheSignsAndTheErrorsMakeSense)
    {
        const Market m;
        const XvaRisks r = risks(m, swaps, false, 4000);
        const std::size_t P = r.factors.size();
        const auto of = [&](XvaOutput o, XvaRiskFactor factor)
        {
            Real sum = 0.0;
            for (std::size_t j = 0; j < P; ++j)
                if (r.factors[j] == factor)
                    sum += r.risks[r.index(o, j)];
            return sum;
        };
        // A riskier counterparty costs more: the CVA, a negative number,
        // falls with its hazard rate and with its loss given default, in
        // which it is linear.
        EXPECT_LT(of(XvaOutput::Cva, XvaRiskFactor::CounterpartyHazard), 0.0);
        EXPECT_NEAR(of(XvaOutput::Cva, XvaRiskFactor::CounterpartyLgd), r[XvaOutput::Cva].value / m.lgd_c,
                    1e-9 * std::abs(r[XvaOutput::Cva].value));
        EXPECT_NEAR(of(XvaOutput::Fca, XvaRiskFactor::BorrowingSpread), r[XvaOutput::Fca].value / m.spread_b,
                    1e-9 * std::abs(r[XvaOutput::Fca].value));
        // More volatility, more exposure on both sides.
        EXPECT_LT(of(XvaOutput::Cva, XvaRiskFactor::Sigma), 0.0);
        EXPECT_GT(of(XvaOutput::Dva, XvaRiskFactor::Sigma), 0.0);
        // The bank's own credit does not enter the unilateral CVA, nor its
        // loss given default the CVA.
        EXPECT_EQ(of(XvaOutput::CvaUnilateral, XvaRiskFactor::OwnHazard), 0.0);
        EXPECT_EQ(of(XvaOutput::Cva, XvaRiskFactor::OwnLgd), 0.0);
        // The bilateral CVA falls (in size) when the bank is riskier: it
        // defaults first more often.
        EXPECT_GT(of(XvaOutput::Cva, XvaRiskFactor::OwnHazard), 0.0);
        // Each sensitivity has its Monte-Carlo error, small against the
        // sensitivity itself for the ones that matter.
        for (std::size_t j = 0; j < P; ++j)
            if (r.factors[j] == XvaRiskFactor::Sigma)
            {
                const Estimate e = r.risk(XvaOutput::Cva, j);
                EXPECT_GT(e.error, 0.0);
                EXPECT_LT(e.error, 0.1 * std::abs(e.value));
            }
        EXPECT_EQ(r.labels.front(), "zero rate 1Y");
        EXPECT_EQ(r.labels[7], "Hull-White mean reversion");
        EXPECT_EQ(r.labels[9], "counterparty hazard rate to 2Y");
        EXPECT_EQ(r.labels[11], "counterparty hazard rate to 10Y");
    }

    TEST(XvaRisks, TheResultDoesNotDependOnTheThreads)
    {
        const Market m;
        const XvaRisks one = risks(m, options, true, 500);
        ThreadPool pool;
        pool.start(3);
        const XvaRisks four = risks(m, options, true, 500, 0.05, &pool);
        pool.stop();
        EXPECT_EQ(four.risks, one.risks);
        EXPECT_EQ(four.errors, one.errors);
        EXPECT_EQ(four.batch_risks, one.batch_risks);
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
            EXPECT_EQ(four.values[o].value, one.values[o].value);
    }

    TEST(XvaRisks, WhatIsNotDifferentiatedIsRefused)
    {
        const Market m;
        const DiscountCurve c = curve(m);
        const HullWhiteCurveModel model(m.a, m.sigma, c);
        HullWhiteExposureEngine engine(model);
        swaps(engine);
        ExposureSimulationSettings historical = settings(100);
        historical.historical = HistoricalRateDynamics{0.2, 0.03, 0.01};
        EXPECT_THROW(engine.xva_risks(historical, inputs(m, false)), InvalidInput);
        XvaRiskInputs bad = inputs(m, false);
        bad.lgd_counterparty = 1.5;
        EXPECT_THROW(engine.xva_risks(settings(100), bad), InvalidInput);
        bad = inputs(m, false);
        bad.smoothing_widths = {1.0};
        EXPECT_THROW(engine.xva_risks(settings(100), bad), InvalidInput);

        // A Bermudan is valued by regression.
        engine.add(BermudanSwaption{make_swap(1.0, 5.0, 0.036, 1, 1, 1e6, true), {1.0, 2.0, 3.0}});
        EXPECT_THROW(engine.xva_risks(settings(100), inputs(m, false)), InvalidInput);

        // Two curves.
        Market shifted = m;
        for (Real &z : shifted.zero)
            z += 0.002;
        const HullWhiteCurveModel two(m.a, m.sigma, c, curve(shifted));
        HullWhiteExposureEngine multi(two);
        swaps(multi);
        EXPECT_THROW(multi.xva_risks(settings(100), inputs(m, false)), InvalidInput);
    }

} // namespace quantModeling
