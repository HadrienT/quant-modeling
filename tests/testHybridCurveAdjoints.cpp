#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/hybrid/hull_white_equity_sim_model.hpp"

#include <cmath>
#include <string>
#include <vector>

/**
 * The curve adjoints of the Hull-White hybrid (issue 313, after lot b2b):
 * the tape differentiates the price to the short rate's sigma and to the
 * curve's factors (CurveFactors). Oracles: the Hull-White closed form with
 * the curve's zero rates or sigma moved (a deterministic function, no
 * Monte-Carlo noise); a zero-coupon's risk to a parallel move is -T times
 * its price on every path; the risks to the pillars add up to the parallel
 * one.
 */

namespace quantModeling
{
    namespace
    {
        using aad::Number;

        const Date kToday = Date::from_iso("2024-06-03");
        const double kA = 0.05, kSigma = 0.012;

        std::vector<double> pillars()
        {
            return {0.5, 1, 2, 3, 5, 7, 10};
        }
        std::vector<double> dfs(double shift = 0.0)
        {
            std::vector<double> out;
            for (double t : pillars())
                out.push_back(std::exp(-(0.03 + 0.0015 * t + shift) * t));
            return out;
        }
        DiscountCurve curve(double shift = 0.0)
        {
            return DiscountCurve(pillars(), dfs(shift), CurveExtrapolation::FlatForward);
        }

        double years(const Date &d)
        {
            return (d - kToday) / 365.0;
        }

        /// One year into five, annual fixed dates, payer at `strike`.
        std::vector<Date> fixed_dates()
        {
            std::vector<Date> out;
            for (int y = 0; y <= 5; ++y)
                out.push_back(kToday + 365 + 365 * y);
            return out;
        }

        std::string swaption_script(double strike)
        {
            const auto d = fixed_dates();
            std::string annuity;
            for (int i = 1; i <= 5; ++i)
                annuity += (i > 1 ? " + " : "") + std::to_string(years(d[i]) - years(d[i - 1])) + " * df(" +
                           d[i].to_iso() + ")";
            return d[0].to_iso() + "\n    pays max(1 - df(" + d[5].to_iso() + ") - " + std::to_string(strike) +
                   " * (" + annuity + "), 0)\n";
        }

        double closed_form(double strike, double shift, double sigma)
        {
            const auto d = fixed_dates();
            InterestRateSwap swap;
            swap.fixed_rate = strike;
            swap.payer = true;
            for (int i = 1; i <= 5; ++i)
            {
                const Time s = years(d[i - 1]), e = years(d[i]);
                swap.fixed_leg.push_back({s, e, e, e - s});
                swap.floating_leg.push_back({s, e, e, e - s});
            }
            return hull_white_european_swaption(Swaption(swap, years(d[0])),
                                                HullWhiteCurveModel(kA, sigma, curve(shift)));
        }

        AADSimulResults run(const std::string &script, CurveFactors factors = {}, long long paths = 200000)
        {
            aad::Tape tape;
            Number::tape = &tape;
            ScriptedProduct<Number> product(script, ValuationContext{kToday});
            HullWhiteEquitySimModel<Number> model(Number(100.0), Number(0.0), Number(0.2),
                                                  HullWhiteCurveModel(kA, kSigma, curve()), 0.0,
                                                  std::move(factors));
            AADSimulResults res = simulate_aad(product, model, paths, 7);
            Number::tape = nullptr;
            return res;
        }

        double risk(const AADSimulResults &r, const std::string &label, double *se = nullptr)
        {
            for (std::size_t i = 0; i < r.risk_labels.size(); ++i)
                if (r.risk_labels[i] == label)
                {
                    if (se)
                        *se = r.risk_std_errors[i];
                    return r.risks[i];
                }
            ADD_FAILURE() << "no risk " << label;
            return 0.0;
        }
    } // namespace

    TEST(HybridCurveAdjoints, ReportsTheRateVolAndOneRiskPerCurveFactor)
    {
        const auto r = run(swaption_script(0.04), {}, 2000);
        const std::vector<std::string> expected{"spot", "div", "vol", "rate_vol", "curve_parallel"};
        EXPECT_EQ(r.risk_labels, expected);
    }

    TEST(HybridCurveAdjoints, AZeroCouponLosesItsMaturityTimesItsPriceOnAParallelMove)
    {
        // Each path pays P(0, T) e^{-I}: d/dz = -T times it, path by path.
        const Date T = kToday + 1500;
        const auto r = run(T.to_iso() + "\n    pays 1\n");
        EXPECT_NEAR(r.price, curve().discount(years(T)), 4.0 * r.price_std_error + 1e-12);
        EXPECT_NEAR(risk(r, "curve_parallel"), -years(T) * r.price, 1e-10);
        double se = 0.0;
        const double vega = risk(r, "rate_vol", &se);
        EXPECT_NEAR(vega, 0.0, 4.0 * se + 1e-12);
    }

    TEST(HybridCurveAdjoints, TheSwaptionsRisksAreTheClosedFormsBumps)
    {
        const double K = 0.04, h = 1e-5;
        const auto r = run(swaption_script(K));
        EXPECT_NEAR(r.price, closed_form(K, 0.0, kSigma), 4.0 * r.price_std_error);

        const double dv_dz = (closed_form(K, h, kSigma) - closed_form(K, -h, kSigma)) / (2 * h);
        const double dv_ds = (closed_form(K, 0.0, kSigma + h) - closed_form(K, 0.0, kSigma - h)) / (2 * h);
        double se_z = 0.0, se_s = 0.0;
        const double z = risk(r, "curve_parallel", &se_z), s = risk(r, "rate_vol", &se_s);
        EXPECT_NEAR(z, dv_dz, 4.0 * se_z) << "closed form " << dv_dz;
        EXPECT_NEAR(s, dv_ds, 4.0 * se_s) << "closed form " << dv_ds;
        EXPECT_GT(dv_ds, 0.0); // a swaption gains with the rate's vol
        EXPECT_GT(dv_dz, 0.0); // a payer gains when rates rise
    }

    TEST(HybridCurveAdjoints, ThePillarRisksAddUpToTheParallelOne)
    {
        // Basis: the identity (one factor per pillar) and a column of ones.
        const std::size_t n = pillars().size(), K = n + 1;
        CurveFactors f;
        f.basis.assign(n * K, 0.0);
        for (std::size_t j = 0; j < n; ++j)
        {
            f.basis[j * K + j] = 1.0;
            f.basis[j * K + n] = 1.0;
            f.labels.push_back("zero[" + std::to_string(j) + "]");
        }
        f.labels.push_back("parallel");
        const auto r = run(swaption_script(0.04), f, 50000);
        double sum = 0.0;
        for (std::size_t j = 0; j < n; ++j)
            sum += risk(r, "zero[" + std::to_string(j) + "]");
        EXPECT_NEAR(sum, risk(r, "parallel"), 1e-10 * std::abs(sum));
        // past the swap's last date nothing moves the price
        EXPECT_EQ(risk(r, "zero[6]"), 0.0);
    }

    TEST(HybridCurveAdjoints, AFactorBasisOfTheWrongShapeIsRefused)
    {
        CurveFactors f{{1.0, 2.0}, {"a"}};
        EXPECT_THROW(HullWhiteEquitySimModel<Real>(100.0, 0.0, 0.2, HullWhiteCurveModel(kA, kSigma, curve()), 0.0, f),
                     InvalidInput);
    }

} // namespace quantModeling
