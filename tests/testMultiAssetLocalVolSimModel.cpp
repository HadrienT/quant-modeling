#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_local_vol_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <Eigen/Core>
#include <cmath>
#include <string>
#include <vector>

// Multi-asset local vol (issue #86): each asset on its own Dupire surface,
// the drivers correlated. Properties: on flat surfaces it is Black-Scholes,
// so an exchange option is Margrabe's; each asset keeps its own marginal,
// whatever the correlation; one asset's surface does not move a payoff on
// another asset at all.

namespace quantModeling
{
    namespace
    {
        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        const std::vector<Real> kK{50, 70, 85, 100, 115, 130, 160, 200};
        const std::vector<Real> kT{0.1, 0.25, 0.5, 1.0, 2.0};

        template <class T>
        std::vector<T> surface(double level, double skew, double slope = 0.01)
        {
            std::vector<T> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(T(level + skew * (100.0 - k) / 100.0 + slope * t));
            return g;
        }

        Eigen::MatrixXd corr2(double rho)
        {
            Eigen::MatrixXd c(2, 2);
            c << 1.0, rho, rho, 1.0;
            return c;
        }

        template <class T>
        MultiAssetLocalVolSimModel<T> two_assets(std::vector<T> s1, std::vector<T> s2, double rho)
        {
            using Surface = typename MultiAssetLocalVolSimModel<T>::Surface;
            return MultiAssetLocalVolSimModel<T>({T(100.0), T(90.0)}, T(0.03), {T(0.01), T(0.02)},
                                                 {Surface{kK, kT, std::move(s1)}, Surface{kK, kT, std::move(s2)}},
                                                 corr2(rho), 1.0 / 100.0);
        }

        SimulationMCResult price(const std::string &script, ISimulationModel<Real> &model, std::size_t paths = 200000)
        {
            const ScriptedProduct<Real> product(script, kCtx);
            PricingSettings set;
            set.mc_paths = paths;
            set.mc_seed = 5;
            return simulate_script(product, model, set);
        }
    } // namespace

    TEST(MultiAssetLocalVol, FlatSurfacesPriceTheExchangeOptionAsMargrabe)
    {
        const double s1 = 0.25, s2 = 0.3, rho = 0.5;
        auto model = two_assets<Real>(surface<Real>(s1, 0.0, 0.0), surface<Real>(s2, 0.0, 0.0), rho);
        const double T = kCtx.t(Date::from_iso("2027-06-01"));
        const double v1 = s1, v2 = s2;
        const double sig = std::sqrt(v1 * v1 + v2 * v2 - 2.0 * rho * v1 * v2);
        const double F1 = 100.0 * std::exp(-0.01 * T), F2 = 90.0 * std::exp(-0.02 * T);
        const double d1 = (std::log(F1 / F2) + 0.5 * sig * sig * T) / (sig * std::sqrt(T));
        const double margrabe = F1 * norm_cdf(d1) - F2 * norm_cdf(d1 - sig * std::sqrt(T));

        const SimulationMCResult res = price("2027-06-01\n    pays max(spot(0) - spot(1), 0)\n", model);
        EXPECT_NEAR(res.npv(), margrabe, 4.0 * res.std_error() + 2e-3 * margrabe);
    }

    TEST(MultiAssetLocalVol, EachAssetKeepsItsOwnMarginal)
    {
        const std::string call = "2027-06-01\n    pays max(spot(1) - 95, 0)\n";
        LocalVolSimModel<Real> alone(90.0, 0.03, 0.02, kK, kT, surface<Real>(0.2, 0.4), 1.0 / 100.0);
        const SimulationMCResult single = price("2027-06-01\n    pays max(spot() - 95, 0)\n", alone);
        for (const double rho : {-0.6, 0.0, 0.8})
        {
            auto model = two_assets<Real>(surface<Real>(0.3, 0.1), surface<Real>(0.2, 0.4), rho);
            const SimulationMCResult joint = price(call, model);
            EXPECT_NEAR(joint.npv(), single.npv(), 4.0 * std::hypot(joint.std_error(), single.std_error()))
                << "rho " << rho;
        }
    }

    TEST(MultiAssetLocalVol, CorrelationMovesTheWorstOf)
    {
        const std::string worst_of = "2027-06-01\n    pays max(min(spot(0) / 100, spot(1) / 90) - 1, 0) * 100\n";
        auto low = two_assets<Real>(surface<Real>(0.25, 0.3), surface<Real>(0.2, 0.4), 0.0);
        auto high = two_assets<Real>(surface<Real>(0.25, 0.3), surface<Real>(0.2, 0.4), 0.9);
        EXPECT_GT(price(worst_of, high, 50000).npv(), price(worst_of, low, 50000).npv());
    }

    // Asset 0's surface does not enter asset 1's path: the adjoint of a
    // payoff on asset 1 to every lvol[0:..] is exactly zero, and its local
    // vegas to asset 1's grid are not.
    TEST(MultiAssetLocalVol, AdjointsStayOnTheirOwnAsset)
    {
        const ScriptedProduct<aad::Number> product("2027-06-01\n    pays max(spot(1) - 95, 0)\n", kCtx);
        auto model = two_assets<aad::Number>(surface<aad::Number>(0.3, 0.1), surface<aad::Number>(0.2, 0.4), 0.6);
        const AADSimulResults res = simulate_aad(product, model, 4096, 3);
        double own = 0.0, other = 0.0;
        for (std::size_t i = 0; i < res.risks.size(); ++i)
        {
            if (res.risk_labels[i].rfind("lvol[0:", 0) == 0)
                other += std::abs(res.risks[i]);
            if (res.risk_labels[i].rfind("lvol[1:", 0) == 0)
                own += std::abs(res.risks[i]);
        }
        EXPECT_EQ(other, 0.0);
        EXPECT_GT(own, 1.0);
        EXPECT_EQ(res.risk_labels.size(), 1 + 2 + 2 + 2 * kK.size() * kT.size());
    }

    TEST(MultiAssetLocalVol, RejectsInconsistentInputs)
    {
        using Surface = MultiAssetLocalVolSimModel<Real>::Surface;
        const Surface ok{kK, kT, surface<Real>(0.2, 0.0)};
        EXPECT_THROW(MultiAssetLocalVolSimModel<Real>({100.0, 90.0}, 0.03, {0.0}, {ok, ok}, corr2(0.1)),
                     InvalidInput);
        EXPECT_THROW(MultiAssetLocalVolSimModel<Real>({100.0, 90.0}, 0.03, {0.0, 0.0}, {ok, Surface{kK, kT, {0.2}}},
                                                      corr2(0.1)),
                     InvalidInput);
        EXPECT_THROW(MultiAssetLocalVolSimModel<Real>({100.0, 90.0}, 0.03, {0.0, 0.0}, {ok, ok}, corr2(1.5)),
                     InvalidInput);
    }

} // namespace quantModeling
