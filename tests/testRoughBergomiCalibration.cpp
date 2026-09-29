#include <gtest/gtest.h>

#include "quantModeling/engines/mc/rough_bergomi_hybrid.hpp"
#include "quantModeling/engines/mc/rough_bergomi_surface.hpp"
#include "quantModeling/market/rough_bergomi_calibration.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {
        ForwardVarianceCurve flat(Real v)
        {
            return {{10.0}, {v}};
        }

        std::vector<RoughBergomiQuote> grid(const std::vector<Time> &ttms, const std::vector<Real> &zs)
        {
            // k in units of the ATM standard deviation at a 20 % vol.
            std::vector<RoughBergomiQuote> q;
            for (const Time T : ttms)
                for (const Real z : zs)
                    q.push_back({T, z * 0.2 * std::sqrt(T)});
            return q;
        }
    } // namespace

    TEST(RoughBergomiSurface, WithoutVolOfVolItIsBlackScholesOnTheForwardVariance)
    {
        // eta = 0: V_t = xi0(t), deterministic, so the implied vol at T is
        // sqrt(∫ xi0 / T) at every strike.
        const ForwardVarianceCurve xi{{0.25, 1.0}, {0.04, 0.09}};
        const auto q = grid({0.25, 1.0}, {-1.0, 0.0, 1.0});
        RoughBergomiSurfaceSettings s;
        s.n_paths = 40000;
        const RoughBergomiSurface r = rough_bergomi_surface({0.1, 0.0, -0.7}, xi, q, s);
        for (std::size_t i = 0; i < q.size(); ++i)
        {
            const Real T = q[i].ttm;
            const Real expected = T <= 0.25 ? 0.2 : std::sqrt((0.04 * 0.25 + 0.09 * 0.75) / T);
            EXPECT_NEAR(r.implied_vols[i], expected, 2e-3) << "T=" << T << " k=" << q[i].k;
        }
    }

    TEST(RoughBergomiSurface, IsTheSameBitsOnOneThreadOrMany)
    {
        const auto q = grid({0.1, 0.5}, {-1.0, 0.5});
        RoughBergomiSurfaceSettings one, many;
        one.n_paths = many.n_paths = 3000;
        one.max_threads = 1;
        many.max_threads = 8;
        const auto a = rough_bergomi_surface({0.1, 1.9, -0.9}, flat(0.04), q, one);
        const auto b = rough_bergomi_surface({0.1, 1.9, -0.9}, flat(0.04), q, many);
        for (std::size_t i = 0; i < q.size(); ++i)
            EXPECT_EQ(a.otm_prices[i], b.otm_prices[i]);
    }

    TEST(RoughBergomiSurface, AgreesWithTheSingleMaturityPricer)
    {
        // engines/mc/rough_bergomi_hybrid.hpp prices one call with its own
        // draws: the two schemes must agree within their Monte-Carlo errors.
        const RoughBergomiParams p{0.1, 1.9, -0.9};
        RoughBergomiSurfaceSettings s;
        s.n_paths = 60000;
        s.steps_per_year = 200;
        const auto surf = rough_bergomi_surface(p, flat(0.04), {{0.5, 0.05}}, s);
        const Real K = std::exp(0.05);
        const auto single = rough_bergomi_price(1.0, K, 0.5, 1.0, p, [](Real)
                                                { return 0.04; }, true,
                                                {100, 60000, 7});
        EXPECT_NEAR(surf.otm_prices[0], single.price, 4.0 * std::hypot(surf.std_errors[0], single.std_error));
    }

    TEST(RoughBergomiSurface, TheAtTheMoneySkewDecaysAsTToTheHMinusOneHalf)
    {
        // The signature of rough volatility (Bayer, Friz & Gatheral 2016):
        // ψ(T) = ∂σ/∂k at the money ~ T^(H - 1/2) for short T. Measured by a
        // log-log regression over maturities from one week to three months.
        const Real H = 0.1;
        const std::vector<Time> ttms{0.02, 0.04, 0.08, 0.16};
        std::vector<RoughBergomiQuote> q;
        std::vector<Real> dk;
        for (const Time T : ttms)
        {
            const Real h = 0.05 * 0.2 * std::sqrt(T);
            dk.push_back(h);
            q.push_back({T, -h});
            q.push_back({T, h});
        }
        RoughBergomiSurfaceSettings s;
        s.n_paths = 60000;
        s.steps_per_year = 1500;
        const auto r = rough_bergomi_surface({H, 1.9, -0.9}, flat(0.04), q, s);
        Real sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t j = 0; j < ttms.size(); ++j)
        {
            const Real skew = (r.implied_vols[2 * j + 1] - r.implied_vols[2 * j]) / (2.0 * dk[j]);
            ASSERT_LT(skew, 0.0);
            const Real x = std::log(ttms[j]), y = std::log(-skew);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const Real n = static_cast<Real>(ttms.size());
        const Real slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        EXPECT_NEAR(slope, H - 0.5, 0.1);
    }

    TEST(RoughBergomiCalibration, RecoversTheParametersThatGeneratedTheSurface)
    {
        // Common random numbers: the targets come from the model on the very
        // paths the calibration prices with, so the true parameters are an
        // exact zero of the objective.
        const RoughBergomiParams truth{0.12, 1.8, -0.75};
        const std::vector<Time> ttms{0.1, 0.25, 0.5};
        const ForwardVarianceCurve xi{{0.1, 0.25, 0.5}, {0.03, 0.035, 0.04}};
        const auto q = grid(ttms, {-1.5, -0.5, 0.0, 0.5, 1.0});
        RoughBergomiSurfaceSettings s;
        s.n_paths = 6000;
        s.steps_per_year = 150;
        const auto surf = rough_bergomi_surface(truth, xi, q, s);
        std::vector<RoughBergomiTarget> targets;
        for (std::size_t i = 0; i < q.size(); ++i)
            targets.push_back({q[i].ttm, q[i].k, surf.implied_vols[i]});
        // Total variances that reproduce xi exactly.
        const std::vector<Real> w{0.003, 0.003 + 0.035 * 0.15, 0.003 + 0.035 * 0.15 + 0.04 * 0.25};
        const auto c = calibrate_rough_bergomi(targets, ttms, w, s, {0.25, 1.0, -0.4});
        EXPECT_NEAR(c.params.H, truth.H, 5e-3);
        EXPECT_NEAR(c.params.eta, truth.eta, 5e-2);
        EXPECT_NEAR(c.params.rho, truth.rho, 2e-2);
        EXPECT_LT(c.iv_rmse, 1e-3);
        EXPECT_EQ(c.n_unpriced, 0u);
    }

} // namespace quantModeling

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/rough_bergomi_sim_model.hpp"

namespace quantModeling
{
    TEST(RoughBergomiSimModel, ACurveModelPricesAMultiDateScriptLikeTheSurfacePricer)
    {
        // A script paying two vanillas at two dates, under the calibrated
        // form (xi0 curve, uniform grid): the sum of the surface pricer's two
        // prices, within Monte-Carlo error.
        const ValuationContext ctx{Date::from_iso("2024-06-03")};
        const std::string script = "2024-09-02\n    pays max(spot() - 100, 0)\n"
                                   "2025-06-03\n    pays max(95 - spot(), 0)\n";
        ScriptedProduct<Real> product(script, ctx);
        const ForwardVarianceCurve xi{{0.25, 1.0}, {0.03, 0.045}};
        RoughBergomiSimModel<Real> model(100.0, 0.0, 0.0, xi, 1.5, -0.8, 0.1, 365);
        PricingSettings s;
        s.mc_paths = 40000;
        s.mc_seed = 3;
        const auto mc = simulate<Real>(product, model, s);

        const Time t1 = 91 / 365.0, t2 = 1.0;
        RoughBergomiSurfaceSettings ss;
        ss.n_paths = 40000;
        ss.steps_per_year = 365;
        const auto surf = rough_bergomi_surface({0.1, 1.5, -0.8}, xi, {{t1, 0.0}, {t2, std::log(0.95)}}, ss);
        const Real expected = 100.0 * (surf.otm_prices[0] + surf.otm_prices[1]);
        const Real se = std::hypot(mc.std_error(), 100.0 * std::hypot(surf.std_errors[0], surf.std_errors[1]));
        EXPECT_NEAR(mc.npv(), expected, 4.0 * se);
    }
} // namespace quantModeling
