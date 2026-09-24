#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantModeling
{

    namespace
    {

        // A CAC 40-like asset (EUR) paid in USD at a fixed rate.
        QuantoBSInput base_input()
        {
            QuantoBSInput in{};
            in.spot = 8000.0;
            in.strike = 8200.0;
            in.maturity = 1.5;
            in.rate_domestic = 0.045; // USD
            in.rate_foreign = 0.030;  // EUR
            in.dividend = 0.025;
            in.vol = 0.18;
            in.fx_vol = 0.08;
            in.correlation = 0.30;
            in.fx_rate = 1.10; // USD per EUR, fixed in advance
            in.is_call = true;
            in.n_paths = 400000;
            in.seed = 7;
            return in;
        }

        PricingResult price_with(const QuantoBSInput &in, EngineKind engine)
        {
            return default_registry().price(
                {InstrumentKind::EquityVanillaOption, ModelKind::QuantoBlackScholes, engine,
                 PricingInput{in}});
        }

        Real Phi(Real x)
        {
            return 0.5 * std::erfc(-x / std::sqrt(2.0));
        }

        /// Reiner (1992), written independently of the model/engine route:
        /// X̄·e^{−r_d T}·[F N(d1) − K N(d2)], F = S·e^{(r_f − q − ρσ_Sσ_X)T}.
        Real reiner(const QuantoBSInput &in)
        {
            const Real T = in.maturity;
            const Real F = in.spot * std::exp((in.rate_foreign - in.dividend -
                                               in.correlation * in.vol * in.fx_vol) *
                                              T);
            const Real sd = in.vol * std::sqrt(T);
            const Real d1 = (std::log(F / in.strike) + 0.5 * sd * sd) / sd;
            const Real d2 = d1 - sd;
            const Real w = in.is_call ? 1.0 : -1.0;
            return in.fx_rate * std::exp(-in.rate_domestic * T) *
                   w * (F * Phi(w * d1) - in.strike * Phi(w * d2));
        }

    } // namespace

    TEST(Quanto, AnalyticMatchesReinersClosedForm)
    {
        for (bool call : {true, false})
            for (Real rho : {-0.8, 0.0, 0.3, 0.9})
            {
                QuantoBSInput in = base_input();
                in.is_call = call;
                in.correlation = rho;
                EXPECT_NEAR(price_with(in, EngineKind::Analytic).npv, reiner(in), 1e-9)
                    << "call=" << call << " rho=" << rho;
            }
    }

    TEST(Quanto, WithoutFxRiskItIsTheVanillaPriceInDomesticUnits)
    {
        // ρ = 0 and r_f = r_d: no drift adjustment, same discounting — the
        // quanto is X̄ times the vanilla Black-Scholes price.
        QuantoBSInput in = base_input();
        in.correlation = 0.0;
        in.rate_foreign = in.rate_domestic;
        VanillaBSInput v{};
        v.spot = in.spot;
        v.strike = in.strike;
        v.maturity = in.maturity;
        v.rate = in.rate_domestic;
        v.dividend = in.dividend;
        v.vol = in.vol;
        v.is_call = true;
        const Real vanilla = default_registry()
                                 .price({InstrumentKind::EquityVanillaOption,
                                         ModelKind::BlackScholes, EngineKind::Analytic,
                                         PricingInput{v}})
                                 .npv;
        EXPECT_NEAR(price_with(in, EngineKind::Analytic).npv, in.fx_rate * vanilla, 1e-9);
    }

    TEST(Quanto, PutCallParityInTheDomesticCurrency)
    {
        // C − P = X̄·e^{−r_d T}·(F_quanto − K)
        QuantoBSInput c = base_input(), p = base_input();
        p.is_call = false;
        const Real T = c.maturity;
        const Real F = c.spot * std::exp((c.rate_foreign - c.dividend -
                                          c.correlation * c.vol * c.fx_vol) *
                                         T);
        const Real lhs = price_with(c, EngineKind::Analytic).npv -
                         price_with(p, EngineKind::Analytic).npv;
        EXPECT_NEAR(lhs, c.fx_rate * std::exp(-c.rate_domestic * T) * (F - c.strike), 1e-8);
    }

    TEST(Quanto, PositiveCorrelationLowersTheCall)
    {
        // ρ > 0 lowers the asset's drift under the domestic measure.
        QuantoBSInput lo = base_input(), hi = base_input();
        lo.correlation = -0.5;
        hi.correlation = 0.5;
        EXPECT_GT(price_with(lo, EngineKind::Analytic).npv,
                  price_with(hi, EngineKind::Analytic).npv);
    }

    TEST(Quanto, MonteCarloAgreesWithTheClosedFormWithinItsError)
    {
        const QuantoBSInput in = base_input();
        const PricingResult mc = price_with(in, EngineKind::MonteCarlo);
        ASSERT_GT(mc.mc_std_error, 0.0);
        EXPECT_NEAR(mc.npv, reiner(in), 4.0 * mc.mc_std_error);
    }

    TEST(Quanto, GreeksAreTheQuantosOwnByFiniteDifferences)
    {
        // The engine holds q_eff fixed; the adapter corrects rho and vega.
        // Every greek is checked against central differences of the price.
        const QuantoBSInput in = base_input();
        const PricingResult res = price_with(in, EngineKind::Analytic);
        auto bumped = [&](auto field, Real h)
        {
            QuantoBSInput up = in, dn = in;
            up.*field += h;
            dn.*field -= h;
            return (price_with(up, EngineKind::Analytic).npv -
                    price_with(dn, EngineKind::Analytic).npv) /
                   (2.0 * h);
        };
        EXPECT_NEAR(*res.greeks.delta, bumped(&QuantoBSInput::spot, 1e-2), 1e-6);
        EXPECT_NEAR(*res.greeks.vega, bumped(&QuantoBSInput::vol, 1e-5), 1e-4);
        EXPECT_NEAR(*res.greeks.rho, bumped(&QuantoBSInput::rate_domestic, 1e-6), 1e-4);
    }

    TEST(Quanto, InvalidInputsAreRefused)
    {
        QuantoBSInput in = base_input();
        in.correlation = 1.2;
        EXPECT_THROW(price_with(in, EngineKind::Analytic), InvalidInput);
        in = base_input();
        in.fx_vol = 0.0;
        EXPECT_THROW(price_with(in, EngineKind::Analytic), InvalidInput);
    }

} // namespace quantModeling
