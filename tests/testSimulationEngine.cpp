#include <gtest/gtest.h>

#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
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

        /// Discretely-monitored geometric-average Asian call, closed form.
        double geo_asian_call(double S0, double K, double r, double q, double v,
                              const TimeLine &t)
        {
            const double n = static_cast<double>(t.size());
            double m = std::log(S0);
            for (double ti : t)
                m += (r - q - 0.5 * v * v) * ti / n;
            double s2 = 0.0;
            for (double ti : t)
                for (double tj : t)
                    s2 += std::min(ti, tj);
            s2 *= (v * v) / (n * n);
            const double s = std::sqrt(s2);
            const double d1 = (m - std::log(K) + s2) / s;
            const double d2 = d1 - s;
            return std::exp(-r * t.back()) *
                   (std::exp(m + 0.5 * s2) * norm_cdf(d1) - K * norm_cdf(d2));
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

        struct GeoAsianCall final : ISimulatableProduct<Real>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            GeoAsianCall(Real k, TimeLine f) : K(k), tl_(std::move(f)), dl_(tl_.size())
            {
            }
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
                double sln = 0.0;
                for (const auto &s : p)
                    sln += std::log(s.spots[0]);
                const double G = std::exp(sln / static_cast<double>(p.size()));
                out.assign(1, std::max(G - K, 0.0) / p.back().numeraire);
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

    TEST(SimulationEngine, EuropeanCallMatchesBlackScholes)
    {
        const double S0 = 100, K = 105, r = 0.03, q = 0.01, v = 0.2, T = 1.5;
        EuroCall prod(K, T);
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());
        const double ref = bs_call(S0, K, r, q, v, T);
        EXPECT_NEAR(res.npv(), ref, 4.0 * res.std_error());
        EXPECT_GT(res.std_error(), 0.0);
        EXPECT_EQ(res.n_paths, 400000);
    }

    TEST(SimulationEngine, GeometricAsianMatchesClosedForm)
    {
        const double S0 = 100, K = 100, r = 0.05, q = 0.0, v = 0.25;
        TimeLine fixings;
        for (int i = 1; i <= 12; ++i)
            fixings.push_back(i / 12.0);
        GeoAsianCall prod(K, fixings);
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());
        EXPECT_NEAR(res.npv(), geo_asian_call(S0, K, r, q, v, fixings),
                    4.0 * res.std_error());
    }

    TEST(SimulationEngine, SobolRqmcMatchesAndIsTighter)
    {
        const double S0 = 100, K = 100, r = 0.04, q = 0.02, v = 0.2, T = 1.0;
        EuroCall prod(K, T);

        PricingSettings pseudo = mc_settings();
        pseudo.mc_paths = 60000;
        BlackScholesSimModel<Real> m1(S0, r, q, v);
        const auto r_pseudo = simulate<Real>(prod, m1, pseudo);

        PricingSettings sobol = pseudo;
        sobol.mc_sampler = SamplerKind::Sobol;
        sobol.mc_rqmc_batches = 20;
        BlackScholesSimModel<Real> m2(S0, r, q, v);
        const auto r_sobol = simulate<Real>(prod, m2, sobol);

        const double ref = bs_call(S0, K, r, q, v, T);
        EXPECT_NEAR(r_sobol.npv(), ref, 4.0 * r_sobol.std_error() + 1e-3);
        EXPECT_LT(r_sobol.std_error(), r_pseudo.std_error());
    }

    TEST(SimulationEngine, AntitheticHalvesTheDrawStream)
    {
        // With antithetic on, path p (odd) is the mirror of path p-1: the mean
        // over an even number of paths is unbiased and the estimate is stable.
        const double S0 = 100, K = 100, r = 0.0, q = 0.0, v = 0.2, T = 1.0;
        EuroCall prod(K, T);
        PricingSettings s = mc_settings();
        s.mc_paths = 200000;
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, s);
        EXPECT_NEAR(res.npv(), bs_call(S0, K, r, q, v, T), 4.0 * res.std_error());
    }

} // namespace quantModeling
