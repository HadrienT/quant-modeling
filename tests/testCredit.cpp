#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/cds.hpp"
#include "quantModeling/market/credit_bootstrap.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/models/credit/intensity.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace quantModeling
{
    namespace
    {
        /// An upward-sloping Treasury-like curve, bootstrapped from par yields,
        /// so the tests do not only run on a flat rate.
        DiscountCurve sloped_curve()
        {
            return bootstrap_curve({{0.25, 0.040}, {0.5, 0.041}},
                                   {make_semiannual_bond_quote(1.0, 0.042),
                                    make_semiannual_bond_quote(2.0, 0.043),
                                    make_semiannual_bond_quote(5.0, 0.045),
                                    make_semiannual_bond_quote(10.0, 0.047)});
        }

        /// Protection leg by brute force: Σ DF(mid) (S(t_k) - S(t_{k+1})) on a
        /// fine grid — independent of the closed-form piecewise integration.
        Real protection_by_quadrature(Time T, const DiscountCurve &dc, const CreditCurve &cc,
                                      Real recovery, int steps)
        {
            Real sum = 0.0;
            const Time h = T / steps;
            for (int k = 0; k < steps; ++k)
            {
                const Time a = k * h, b = a + h;
                sum += dc.discount(0.5 * (a + b)) * (cc.survival(a) - cc.survival(b));
            }
            return (1.0 - recovery) * sum;
        }
    } // namespace

    // ── CreditCurve ──────────────────────────────────────────────────────────

    TEST(CreditCurve, FlatHazardGivesExponentialSurvival)
    {
        const CreditCurve c(0.02);
        EXPECT_NEAR(c.survival(5.0), std::exp(-0.1), 1e-15);
        EXPECT_NEAR(c.survival(30.0), std::exp(-0.6), 1e-15);
        EXPECT_DOUBLE_EQ(c.survival(0.0), 1.0);
        EXPECT_DOUBLE_EQ(c.hazard(12.0), 0.02);
    }

    TEST(CreditCurve, PiecewiseSurvivalIsContinuousAndIntegratesTheHazard)
    {
        const CreditCurve c({1.0, 3.0, 5.0}, {0.01, 0.02, 0.04});
        EXPECT_NEAR(c.survival(1.0), std::exp(-0.01), 1e-15);
        EXPECT_NEAR(c.survival(3.0), std::exp(-0.01 - 0.04), 1e-15);
        EXPECT_NEAR(c.survival(4.0), std::exp(-0.05 - 0.04), 1e-15);
        EXPECT_NEAR(c.survival(7.0), std::exp(-0.05 - 0.08 - 0.08), 1e-15); // flat past 5Y
        EXPECT_NEAR(c.survival(3.0 - 1e-12), c.survival(3.0 + 1e-12), 1e-12);
        EXPECT_DOUBLE_EQ(c.hazard(3.0), 0.02); // right end belongs to its segment
        EXPECT_DOUBLE_EQ(c.hazard(3.5), 0.04);
        EXPECT_NEAR(c.conditional_default_probability(3.0, 5.0), 1.0 - std::exp(-0.08), 1e-14);
    }

    TEST(CreditCurve, RejectsNegativeHazardAndBadPillars)
    {
        EXPECT_THROW(CreditCurve({1.0, 2.0}, {0.01, -0.001}), InvalidInput);
        EXPECT_THROW(CreditCurve({1.0, 1.0}, {0.01, 0.01}), InvalidInput);
        EXPECT_THROW(CreditCurve({}, {}), InvalidInput);
    }

    // ── CDS legs ─────────────────────────────────────────────────────────────

    TEST(CDS, ScheduleRollsBackFromMaturityWithAShortFirstPeriod)
    {
        const CreditDefaultSwap cds = make_cds(1.1, 0.01, 4);
        ASSERT_EQ(cds.payment_times.size(), 5u);
        EXPECT_NEAR(cds.payment_times.front(), 0.1, 1e-12);
        EXPECT_NEAR(cds.accruals.front(), 0.1, 1e-12);
        EXPECT_NEAR(cds.payment_times.back(), 1.1, 1e-15);
        Real total = 0.0;
        for (Real a : cds.accruals)
            total += a;
        EXPECT_NEAR(total, 1.1, 1e-12);
    }

    TEST(CDS, NoDefaultRiskMeansNoProtectionAndARisklessAnnuity)
    {
        const DiscountCurve dc = sloped_curve();
        const CreditDefaultSwap cds = make_cds(5.0, 0.01);
        const CdsLegs legs = cds_legs(cds, dc, CreditCurve(0.0), 0.4);
        Real annuity = 0.0;
        for (std::size_t i = 0; i < cds.payment_times.size(); ++i)
            annuity += cds.accruals[i] * dc.discount(cds.payment_times[i]);
        EXPECT_DOUBLE_EQ(legs.protection, 0.0);
        EXPECT_NEAR(legs.risky_annuity, annuity, 1e-14);
    }

    // The credit triangle s = λ (1 - R) holds exactly for a flat hazard at a
    // zero rate, even with quarterly payments: with accrued-on-default the
    // premium leg of each period is ∫ S(t) dt (integration by parts).
    TEST(CDS, FlatHazardAtZeroRateParSpreadIsExactlyTheCreditTriangle)
    {
        const DiscountCurve dc(0.0);
        for (const Real lambda : {0.005, 0.02, 0.08})
        {
            const CdsLegs legs = cds_legs(make_cds(5.0, 0.0, 4), dc, CreditCurve(lambda), 0.4);
            EXPECT_NEAR(legs.par_spread(), lambda * 0.6, 1e-14) << lambda;
        }
    }

    // At a positive rate, premiums paid at the end of each period instead of
    // continuously lift the spread by ≈ r Δ / 2 relative: the error against
    // the triangle is first order in the payment period Δ, so going from
    // quarterly to weekly divides it by 13.
    TEST(CDS, CreditTriangleErrorIsFirstOrderInThePaymentPeriod)
    {
        const DiscountCurve dc = sloped_curve();
        for (const Real lambda : {0.005, 0.02, 0.08})
        {
            const Real target = lambda * 0.6;
            const Real quarterly =
                cds_legs(make_cds(5.0, 0.0, 4), dc, CreditCurve(lambda), 0.4).par_spread() - target;
            const Real weekly =
                cds_legs(make_cds(5.0, 0.0, 52), dc, CreditCurve(lambda), 0.4).par_spread() - target;
            EXPECT_GT(quarterly, 0.0);
            EXPECT_NEAR(quarterly / weekly, 13.0, 0.1) << lambda;
            EXPECT_LT(quarterly / target, 0.01); // under 1% of the spread
        }
    }

    TEST(CDS, ProtectionLegMatchesBruteForceQuadrature)
    {
        const DiscountCurve dc = sloped_curve();
        const CreditCurve cc({1.0, 3.0, 7.0}, {0.01, 0.025, 0.04});
        const CdsLegs legs = cds_legs(make_cds(7.0, 0.0), dc, cc, 0.35);
        EXPECT_NEAR(legs.protection, protection_by_quadrature(7.0, dc, cc, 0.35, 200000), 1e-9);
    }

    TEST(CDS, BuyerAndSellerAreMirrorImages)
    {
        const DiscountCurve dc(0.03);
        const CreditCurve cc(0.03);
        const CreditDefaultSwap buyer = make_cds(5.0, 0.01, 4, 1e7, true);
        const CreditDefaultSwap seller = make_cds(5.0, 0.01, 4, 1e7, false);
        const CdsLegs legs = cds_legs(buyer, dc, cc, 0.4);
        EXPECT_DOUBLE_EQ(cds_npv(buyer, legs), -cds_npv(seller, legs));
        EXPECT_GT(cds_npv(buyer, legs), 0.0); // 100bp paid for ~180bp of risk
    }

    TEST(CDS, EngineThroughTheVisitorMatchesTheLegs)
    {
        const DiscountCurve dc = sloped_curve();
        const CreditCurve cc({2.0, 5.0}, {0.015, 0.03});
        auto model = std::make_shared<IntensityModel>(dc, cc, 0.4);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        CdsAnalyticEngine engine(ctx);
        const CreditDefaultSwap cds = make_cds(5.0, 0.012, 4, 1e6);
        cds.accept(engine);
        EXPECT_NEAR(engine.results().npv, cds_npv(cds, cds_legs(cds, dc, cc, 0.4)), 1e-9);
    }

    // ── Bootstrap ────────────────────────────────────────────────────────────

    // The defining property: every input CDS reprices to zero at its spread.
    TEST(CreditBootstrap, EveryQuoteRepricesAtPar)
    {
        const DiscountCurve dc = sloped_curve();
        const std::vector<CdsQuote> quotes = {
            {1.0, 0.0060}, {3.0, 0.0085}, {5.0, 0.0110}, {7.0, 0.0125}, {10.0, 0.0140}};
        const CreditCurve cc = bootstrap_credit_curve(quotes, dc, 0.4);
        ASSERT_EQ(cc.times().size(), quotes.size());
        for (const CdsQuote &q : quotes)
        {
            const CdsLegs legs = cds_legs(make_cds(q.maturity, q.spread), dc, cc, 0.4);
            EXPECT_NEAR(legs.par_spread(), q.spread, 1e-12) << q.maturity;
        }
        for (Real h : cc.hazards())
            EXPECT_GE(h, 0.0);
        EXPECT_GT(cc.survival(3.0), cc.survival(5.0));
    }

    TEST(CreditBootstrap, AFlatSpreadCurveGivesAFlatHazard)
    {
        const DiscountCurve dc(0.04);
        const CreditCurve cc =
            bootstrap_credit_curve({{1.0, 0.01}, {3.0, 0.01}, {5.0, 0.01}, {10.0, 0.01}}, dc, 0.4);
        for (Real h : cc.hazards())
            EXPECT_NEAR(h, cc.hazards().front(), 2e-6);
        EXPECT_NEAR(flat_hazard_from_spread(5.0, 0.01, dc, 0.4), 0.01 / 0.6, 1e-4);
    }

    TEST(CreditBootstrap, ASteeplyFallingCurveIsReportedNotFloored)
    {
        const DiscountCurve dc(0.04);
        EXPECT_THROW(bootstrap_credit_curve({{1.0, 0.05}, {2.0, 0.005}}, dc, 0.4), InvalidInput);
        EXPECT_THROW(bootstrap_credit_curve({{1.0, 0.01}, {1.0, 0.02}}, dc, 0.4), InvalidInput);
        EXPECT_THROW(bootstrap_credit_curve({}, dc, 0.4), InvalidInput);
    }

    // A higher recovery assumption must be compensated by more default risk
    // to produce the same spread: λ ≈ s / (1 - R).
    TEST(CreditBootstrap, HazardRisesWithTheRecoveryAssumption)
    {
        const DiscountCurve dc(0.04);
        const Real low = flat_hazard_from_spread(5.0, 0.012, dc, 0.2);
        const Real high = flat_hazard_from_spread(5.0, 0.012, dc, 0.6);
        EXPECT_GT(high, low);
        EXPECT_NEAR(high / low, 0.8 / 0.4, 0.01);
    }

} // namespace quantModeling
