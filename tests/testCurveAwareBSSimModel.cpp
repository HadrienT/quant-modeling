#include <gtest/gtest.h>

#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        double bs_call(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            const double d2 = d1 - sd;
            return S * std::exp(-q * T) * norm_cdf(d1) -
                   K * std::exp(-r * T) * norm_cdf(d2);
        }

        struct EuroCall final : ISimulatableProduct<Real>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            EuroCall(Real k, Real T) : K(k), tl_{T}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<Real> &p,
                         std::vector<Real> &out) const override
            {
                out.assign(1, std::max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };

        PricingSettings mc_settings()
        {
            PricingSettings s;
            s.mc_paths = 400000;
            s.mc_seed = 20240101;
            s.mc_antithetic = true;
            return s;
        }
    } // namespace

    // A flat DiscountCurve must reproduce the flat-rate constructor exactly:
    // this is the sanity check that curve-based and flat-rate drift/discount/
    // numeraire/forward formulas are the same formula written two ways, not
    // two independent implementations that happen to agree on average.
    TEST(CurveAwareBSSimModel, FlatCurveMatchesFlatRateConstructorExactly)
    {
        const double S0 = 100, K = 105, r = 0.03, q = 0.01, v = 0.2, T = 1.5;
        const DiscountCurve flat(r);

        EuroCall prod(K, T);
        BlackScholesSimModel<Real> flat_model(S0, r, q, v);
        BlackScholesSimModel<Real> curve_model(S0, flat, q, v);

        const auto res_flat = simulate<Real>(prod, flat_model, mc_settings());
        const auto res_curve = simulate<Real>(prod, curve_model, mc_settings());

        // Same seed, same antithetic scheme, same formula in different guise
        // => bit-identical Monte-Carlo estimate, not merely "close".
        EXPECT_DOUBLE_EQ(res_flat.npv(), res_curve.npv());
    }

    TEST(CurveAwareBSSimModel, EuropeanCallMatchesBlackScholesUnderAFlatCurve)
    {
        const double S0 = 100, K = 95, r = 0.025, q = 0.0, v = 0.22, T = 2.0;
        const DiscountCurve curve(r);

        EuroCall prod(K, T);
        BlackScholesSimModel<Real> model(S0, curve, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());
        const double ref = bs_call(S0, K, r, q, v, T);
        EXPECT_NEAR(res.npv(), ref, 4.0 * res.std_error());
    }

    // The point of a curve-aware model: an upward-sloping term structure
    // should discount/drift a forward-starting payoff at the *forward* rate
    // over [0, T], not at some single flat number — the exact gap a cliquet
    // needs closed. Reprice against Black-Scholes fed the curve's own
    // zero rate at T (r_eff = -ln DF(T) / T), the one number a flat model
    // would have to be told to reproduce the same price.
    TEST(CurveAwareBSSimModel, EuropeanCallUnderASlopedBootstrappedCurve)
    {
        const double S0 = 100, K = 100, q = 0.0, v = 0.2, T = 3.0;
        const std::vector<DepositQuote> deposits = {{0.25, 0.02}};
        const std::vector<ParRateQuote> bonds = {
            make_semiannual_bond_quote(1.0, 0.025),
            make_semiannual_bond_quote(2.0, 0.03),
            make_semiannual_bond_quote(3.0, 0.035),
        };
        const DiscountCurve curve = bootstrap_curve(deposits, bonds);
        const double r_eff = -std::log(curve.discount(T)) / T;

        EuroCall prod(K, T);
        BlackScholesSimModel<Real> model(S0, curve, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());
        const double ref = bs_call(S0, K, r_eff, q, v, T);
        EXPECT_NEAR(res.npv(), ref, 4.0 * res.std_error());

        // and it must differ meaningfully from pricing off the 3Y par rate
        // taken as a flat number — the mistake a naive "flat r" cliquet makes
        EXPECT_GT(std::fabs(res.npv() - bs_call(S0, K, 0.035, q, v, T)), 1e-3);
    }

} // namespace quantModeling
