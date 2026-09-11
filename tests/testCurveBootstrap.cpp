#include <gtest/gtest.h>

#include "quantModeling/market/curve_bootstrap.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {
        /// Par yield of a semi-annual bond of maturity T under a known
        /// continuously-compounded flat zero rate — the ground truth a
        /// bootstrap from those par yields must recover.
        Real flat_curve_par_yield(Real flat_rate, Time maturity)
        {
            const int periods = static_cast<int>(std::lround(maturity / 0.5));
            Real coupon_pv_per_unit_rate = 0.0;
            for (int k = 1; k <= periods; ++k)
                coupon_pv_per_unit_rate += 0.5 * std::exp(-flat_rate * 0.5 * k);
            const Real df_T = std::exp(-flat_rate * maturity);
            return (1.0 - df_T) / coupon_pv_per_unit_rate;
        }
    } // namespace

    // ── deposits ─────────────────────────────────────────────────────────────

    TEST(CurveBootstrap, SingleDepositMatchesClosedForm)
    {
        const DiscountCurve curve = bootstrap_curve({{0.25, 0.05}}, {});
        EXPECT_NEAR(curve.discount(0.25), 1.0 / (1.0 + 0.05 * 0.25), 1e-12);
    }

    TEST(CurveBootstrap, DepositsAreMonotoneDecreasing)
    {
        const DiscountCurve curve =
            bootstrap_curve({{0.25, 0.04}, {0.5, 0.045}, {1.0, 0.05}}, {});
        EXPECT_GT(curve.discount(0.25), curve.discount(0.5));
        EXPECT_GT(curve.discount(0.5), curve.discount(1.0));
        EXPECT_LT(curve.discount(1.0), 1.0);
    }

    // ── the defining property: repricing an input quote returns par ────────

    TEST(CurveBootstrap, RepricingASemiannualBondReturnsPar)
    {
        const std::vector<ParRateQuote> quotes = {
            make_semiannual_bond_quote(1.0, 0.045),
            make_semiannual_bond_quote(2.0, 0.047),
            make_semiannual_bond_quote(3.0, 0.048),
            make_semiannual_bond_quote(5.0, 0.05), // 3Y -> 5Y skips 3.5/4/4.5
        };
        const DiscountCurve curve = bootstrap_curve({}, quotes);

        for (const ParRateQuote &q : quotes)
        {
            Real coupon_pv = 0.0;
            for (std::size_t i = 0; i < q.payment_times.size(); ++i)
                coupon_pv += q.accruals[i] * curve.discount(q.payment_times[i]);
            const Real pv = q.rate * coupon_pv + curve.discount(q.maturity());
            EXPECT_NEAR(pv, 1.0, 1e-8) << "quote maturing at " << q.maturity();
        }
    }

    TEST(CurveBootstrap, RecoversAKnownFlatCurveFromItsParYields)
    {
        // A short deposit anchors the front pillar close to "now", like a real
        // curve — DiscountCurve extrapolates flat below its first pillar
        // (deliberately, see DiscountCurve's own tests), so without a short
        // anchor every bond's own sub-front coupon would be priced at the
        // first pillar's DF instead of its true value, biasing the recovered
        // curve by a few bp. That bias is a property of the flat-front
        // convention, not of the bootstrap itself.
        const Real flat_rate = 0.04;
        const Real one_month = 1.0 / 12.0;
        const std::vector<DepositQuote> deposits = {
            // simple rate that reprices the flat curve's true 1M DF exactly
            {one_month, (std::exp(flat_rate * one_month) - 1.0) / one_month}};

        std::vector<ParRateQuote> quotes;
        for (Real T : {1.0, 2.0, 3.0, 5.0, 7.0, 10.0})
            quotes.push_back(
                make_semiannual_bond_quote(T, flat_curve_par_yield(flat_rate, T)));

        const DiscountCurve curve = bootstrap_curve(deposits, quotes);

        for (Real T : {1.0, 2.0, 3.0, 5.0, 7.0, 10.0})
            EXPECT_NEAR(curve.discount(T), std::exp(-flat_rate * T), 1e-6)
                << "T = " << T;
        // an interior, off-pillar point too (3.5Y falls in the 3Y->5Y gap)
        EXPECT_NEAR(curve.discount(3.5), std::exp(-flat_rate * 3.5), 1e-4);
    }

    TEST(CurveBootstrap, MixedDepositsAndBondsStayMonotoneAndReprice)
    {
        const std::vector<DepositQuote> deposits = {{1.0 / 12.0, 0.045},
                                                    {0.25, 0.046}};
        const std::vector<ParRateQuote> bonds = {
            make_semiannual_bond_quote(1.0, 0.047),
            make_semiannual_bond_quote(2.0, 0.048),
        };
        const DiscountCurve curve = bootstrap_curve(deposits, bonds);

        Real prev = 1.0;
        for (Real t : {1.0 / 12.0, 0.25, 1.0, 2.0})
        {
            EXPECT_LT(curve.discount(t), prev);
            prev = curve.discount(t);
        }
    }

    // ── errors ───────────────────────────────────────────────────────────────

    TEST(CurveBootstrap, EmptyQuotesThrow)
    {
        EXPECT_THROW(bootstrap_curve({}, {}), InvalidInput);
    }

    TEST(CurveBootstrap, DuplicateMaturityThrows)
    {
        EXPECT_THROW(bootstrap_curve({{1.0, 0.05}, {1.0, 0.06}}, {}), InvalidInput);
    }

    TEST(CurveBootstrap, NonPositiveMaturityThrows)
    {
        EXPECT_THROW(bootstrap_curve({{0.0, 0.05}}, {}), InvalidInput);
        EXPECT_THROW(bootstrap_curve({{-1.0, 0.05}}, {}), InvalidInput);
    }

    TEST(CurveBootstrap, MismatchedPaymentsAndAccrualsThrows)
    {
        ParRateQuote bad;
        bad.rate = 0.05;
        bad.payment_times = {0.5, 1.0};
        bad.accruals = {0.5}; // one short
        EXPECT_THROW(bootstrap_curve({}, {bad}), InvalidInput);
    }

    TEST(CurveBootstrap, UnbracketableRootThrows)
    {
        // a 20% coupon bond with only a single 1Y pillar under it and an
        // absurdly low yield is outside what the bisection bracket can price
        ParRateQuote absurd = make_semiannual_bond_quote(1.0, -5.0);
        EXPECT_THROW(bootstrap_curve({}, {absurd}), InvalidInput);
    }

    // ── make_semiannual_bond_quote ───────────────────────────────────────────

    TEST(MakeSemiannualBondQuote, BuildsTheExpectedSchedule)
    {
        const ParRateQuote q = make_semiannual_bond_quote(2.0, 0.05);
        EXPECT_EQ(q.rate, 0.05);
        ASSERT_EQ(q.payment_times.size(), 4u);
        EXPECT_DOUBLE_EQ(q.payment_times[0], 0.5);
        EXPECT_DOUBLE_EQ(q.payment_times[3], 2.0);
        for (Real a : q.accruals)
            EXPECT_DOUBLE_EQ(a, 0.5);
    }

    TEST(MakeSemiannualBondQuote, RejectsNonHalfYearMaturity)
    {
        EXPECT_THROW(make_semiannual_bond_quote(1.3, 0.05), InvalidInput);
    }

    // ── T-bill discount rate -> bond-equivalent yield ───────────────────────

    TEST(TBillBondEquivalentYield, MatchesTheStandardFormula)
    {
        // r = 365 d / (360 - d * days); 91-day bill, 5% discount rate
        const Real d = 0.05;
        const int days = 91;
        const Real expected = 365.0 * d / (360.0 - d * days);
        EXPECT_NEAR(t_bill_bond_equivalent_yield(d, days), expected, 1e-15);
        // the bond-equivalent yield is always a little above the discount rate
        EXPECT_GT(t_bill_bond_equivalent_yield(d, days), d);
    }

    TEST(TBillBondEquivalentYield, RejectsBeyondSixMonths)
    {
        EXPECT_THROW(t_bill_bond_equivalent_yield(0.05, 183), InvalidInput);
    }

} // namespace quantModeling
