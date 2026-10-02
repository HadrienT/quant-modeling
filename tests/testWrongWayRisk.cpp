#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/wrong_way_risk.hpp"
#include "quantModeling/risk/xva_report.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X6 of blueprint/wp/23-xva.md: wrong-way risk by Hull & White (2012),
// a hazard rate that depends on the value of the netting set.

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
            const Real par =
                value_swap(make_swap(0.0, tenor, 0.0, 1, 1), MultiCurve{m.discount(), m.projection()})
                    .par_rate;
            return make_swap(0.0, tenor, par, 1, 1, notional, payer);
        }

        /// A payer 10Y of 100 and a receiver 5Y of 40.
        ExposurePaths book(const HullWhiteCurveModel &m, std::size_t paths = 10000, Real side = 1.0,
                           Time mpor = 0.0)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(swap(m, 10.0, true, 100.0), side);
            engine.add(swap(m, 5.0, false, 40.0), side);
            ExposureSimulationSettings settings;
            settings.paths = paths;
            settings.grid.margin_period_of_risk = mpor;
            return engine.simulate(settings);
        }

        XvaInputs inputs(Real b = 0.0)
        {
            XvaInputs in;
            in.counterparty = CreditCurve({2.0, 10.0}, {0.02, 0.035});
            in.own = CreditCurve(0.012);
            in.lgd_counterparty = 0.6;
            in.lgd_own = 0.55;
            in.wrong_way_b = b;
            return in;
        }

        /// The survival averaged over the paths, date by date.
        std::vector<Real> model_survival(const ExposurePaths &paths, const PathwiseSurvival &q)
        {
            const std::size_t n = paths.dates(), N = paths.paths;
            std::vector<Real> out(n, 0.0);
            for (std::size_t i = 0; i < n; ++i)
            {
                for (std::size_t p = 0; p < N; ++p)
                    out[i] += q.survival[p * n + i];
                out[i] /= static_cast<Real>(N);
            }
            return out;
        }
    } // namespace

    TEST(WrongWayRisk, TheAverageSurvivalIsTheMarketsWhateverB)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m);
        const CreditCurve curve = inputs().counterparty;
        for (const Real b : {-0.2, -0.05, 0.05, 0.2})
        {
            const PathwiseSurvival q = wrong_way_survival(paths, {}, curve, b);
            ASSERT_EQ(q.a.size(), paths.dates());
            const std::vector<Real> s = model_survival(paths, q);
            for (std::size_t i = 0; i < s.size(); ++i)
                ASSERT_NEAR(s[i], curve.survival(paths.times[i]), 1e-10) << "b=" << b;
            // A survival probability: in (0, 1], falling along each path.
            const std::size_t n = paths.dates();
            for (std::size_t p = 0; p < paths.paths; p += 97)
                for (std::size_t i = 0; i < n; ++i)
                {
                    ASSERT_GE(q.survival[p * n + i], 0.0);
                    ASSERT_LE(q.survival[p * n + i], q.before(p, i));
                }
        }
    }

    TEST(WrongWayRisk, WithoutDependenceEveryPathHasTheMarketsSurvival)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 2000);
        const CreditCurve curve = inputs().counterparty;
        const PathwiseSurvival q = wrong_way_survival(paths, {}, curve, 0.0);
        EXPECT_TRUE(q.a.empty());
        const std::size_t n = paths.dates();
        for (std::size_t p = 0; p < paths.paths; p += 211)
            for (std::size_t i = 0; i < n; ++i)
                ASSERT_EQ(q.survival[p * n + i], curve.survival(paths.times[i]));

        // b = 0 in the report is lot X3, to the bit.
        const XvaReport independent = xva_report(paths, inputs());
        EXPECT_EQ(independent.cva.value, independent.cva_independent.value);
        EXPECT_EQ(independent.dva.value, independent.dva_independent.value);
        EXPECT_EQ(independent.cva.error, independent.cva_independent.error);
        // And the dependence comes in continuously: a tiny b, a tiny change.
        const XvaReport almost = xva_report(paths, inputs(1e-9));
        EXPECT_EQ(almost.cva_independent.value, independent.cva.value);
        EXPECT_NEAR(almost.cva.value, independent.cva.value, 1e-7 * std::abs(independent.cva.value));
        EXPECT_NEAR(almost.dva.value, independent.dva.value, 1e-7 * std::abs(independent.dva.value));
    }

    TEST(WrongWayRisk, TheHazardIsHigherWhereTheCounterpartyOwesMore)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m);
        const CreditCurve curve = inputs().counterparty;
        const std::size_t n = paths.dates(), N = paths.paths;
        std::size_t i = 0;
        while (paths.times[i] < 5.0 - 1e-9)
            ++i;
        // Split the paths by the value of the set at five years.
        const auto conditional = [&](Real b)
        {
            const PathwiseSurvival q = wrong_way_survival(paths, {}, curve, b);
            Real owed = 0.0, owing = 0.0;
            std::size_t n_owed = 0, n_owing = 0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real v = paths.trade_values[0][p * n + i] + paths.trade_values[1][p * n + i];
                // Default probability of the period ending at t_i.
                const Real pd = q.before(p, i) - q.survival[p * n + i];
                (v > 0.0 ? owed : owing) += pd;
                ++(v > 0.0 ? n_owed : n_owing);
            }
            return std::pair{owed / static_cast<Real>(n_owed), owing / static_cast<Real>(n_owing)};
        };
        const auto [wrong_up, wrong_down] = conditional(0.1);
        const auto [right_up, right_down] = conditional(-0.1);
        EXPECT_GT(wrong_up, 1.2 * wrong_down); // wrong-way: defaults when it owes
        EXPECT_LT(right_up, right_down / 1.2); // right-way: defaults when it is owed
    }

    TEST(WrongWayRisk, CvaAndDvaBothGrowWithB)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 5000);
        std::vector<Real> cva, dva;
        const std::vector<Real> bs = {-0.1, -0.05, -0.02, 0.0, 0.02, 0.05, 0.1};
        for (const Real b : bs)
        {
            const XvaReport r = xva_report(paths, inputs(b));
            cva.push_back(r.cva.value);
            dva.push_back(r.dva.value);
            EXPECT_LT(r.cva.value, 0.0);
            EXPECT_GT(r.cva.error, 0.0);
            // The independent figures do not depend on b.
            EXPECT_EQ(r.cva_independent.value, xva_report(paths, inputs()).cva.value);
        }
        // More default where the exposure is: a larger cost, monotonically.
        for (std::size_t k = 1; k < bs.size(); ++k)
        {
            EXPECT_LT(cva[k], cva[k - 1]) << "b=" << bs[k];
            // The DVA grows too: the bank's own default counts on the paths
            // where it owes, and with b > 0 the counterparty survives longer
            // exactly there, so the bank is more often the first to default.
            EXPECT_GT(dva[k], dva[k - 1]) << "b=" << bs[k];
        }
        // The ratio to publish (blueprint §8.2): CVA with wrong-way risk over
        // the independent CVA.
        EXPECT_LT(cva.front() / cva[3], 0.8);
        EXPECT_GT(cva.back() / cva[3], 1.2);
    }

    TEST(WrongWayRisk, SeenFromTheOtherSideWrongWayIsRightWay)
    {
        // The counterparty holds the mirror book. Its hazard rising with the
        // bank's value is its hazard falling with its own value: b changes
        // sign with the side, and the survival of each path is the same.
        const HullWhiteCurveModel m = model();
        const ExposurePaths ours = book(m, 3000), theirs = book(m, 3000, -1.0);
        const CreditCurve curve = inputs().counterparty;
        const PathwiseSurvival a = wrong_way_survival(ours, {}, curve, 0.1);
        const PathwiseSurvival b = wrong_way_survival(theirs, {}, curve, -0.1);
        for (std::size_t c = 0; c < a.survival.size(); c += 53)
            ASSERT_NEAR(a.survival[c], b.survival[c], 1e-12);
    }

    TEST(WrongWayRisk, UnderACsaTheExposureIsAMoveAndTheLevelOfTheValueHardlyMatters)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 5000, 1.0, kTenDays);
        XvaInputs in = inputs(0.1);
        Csa csa;
        csa.margin_period_of_risk = kTenDays;
        in.csa = csa;
        const XvaReport collateralised = xva_report(paths, in);
        const XvaReport open_set = xva_report(paths, inputs(0.1));
        const Real with_csa = collateralised.cva.value / collateralised.cva_independent.value;
        const Real without = open_set.cva.value / open_set.cva_independent.value;
        // Without collateral the exposure is the value itself: a hazard that
        // rises with it doubles the CVA.
        EXPECT_GT(without, 1.8);
        // Under a perfect CSA the exposure is the move of the value over ten
        // days, which is no larger where the value is high: the same hazard
        // changes the CVA by a fraction of that, and not upwards (those are
        // the scenarios of high rates, where a move is discounted more).
        EXPECT_GT(with_csa, 0.7);
        EXPECT_LT(with_csa, 1.05);
        EXPECT_LT(collateralised.cva.value, 0.0);
        // Each trade has its figures; no Euler share under wrong-way risk.
        const XvaReport &o = open_set;
        ASSERT_EQ(o.contributions.size(), 2u);
        for (const TradeContribution &c : o.contributions)
        {
            EXPECT_TRUE(std::isfinite(c.standalone_cva));
            EXPECT_TRUE(std::isfinite(c.incremental_cva));
            EXPECT_TRUE(std::isnan(c.marginal_cva));
        }
        // The payer alone is wrong-way with b > 0: dearer than independent.
        XvaInputs payer = inputs();
        payer.trades = {0};
        EXPECT_LT(o.contributions[0].standalone_cva, xva_report(paths, payer).cva.value);
    }

    TEST(WrongWayRisk, RejectsWhatItCannotCalibrate)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = book(m, 500);
        const CreditCurve curve = inputs().counterparty;
        EXPECT_THROW(wrong_way_survival(paths, {}, curve, std::nan("")), InvalidInput);
        EXPECT_THROW(wrong_way_survival(paths, {7}, curve, 0.1), InvalidInput);
        ExposurePaths historical = paths;
        historical.measure = ExposureMeasure::Historical;
        EXPECT_THROW(wrong_way_survival(historical, {}, curve, 0.1), InvalidInput);
        // A b so large that the default probability of a period would have
        // to sit on a handful of paths: refused, and nothing overflows on
        // the way.
        EXPECT_THROW(wrong_way_survival(paths, {}, curve, 50.0), InvalidInput);
        // A survival curve cannot rise; a flat one (no default) is fine.
        const PathwiseSurvival safe = wrong_way_survival(paths, {}, CreditCurve(0.0), 0.1);
        for (const Real q : safe.survival)
            ASSERT_NEAR(q, 1.0, 1e-9);
    }

} // namespace quantModeling
