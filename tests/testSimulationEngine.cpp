#include <gtest/gtest.h>

#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/equity/simulatable_asian.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/merton_jump_diffusion_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

            EuroCall(Real k, Real T)
                : K(k), tl_{T}, dl_(1) {}
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

            GeoAsianCall(Real k, TimeLine f)
                : K(k), tl_(std::move(f)), dl_(tl_.size())
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

    TEST(SimulatableAsian, GeometricMatchesClosedForm)
    {
        const double S0 = 100, K = 95, r = 0.03, q = 0.01, v = 0.3;
        TimeLine fixings;
        for (int i = 1; i <= 10; ++i)
            fixings.push_back(0.1 * i);

        SimulatableAsian<Real> prod(fixings, K, /*is_call=*/true, /*geometric=*/true);
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());
        EXPECT_NEAR(res.npv(), geo_asian_call(S0, K, r, q, v, prod.timeline()),
                    4.0 * res.std_error());
    }

    TEST(SimulatableAsian, ArithmeticDominatesGeometric)
    {
        // AM >= GM pointwise, so the arithmetic-average call is worth at least
        // the geometric-average call at the same strike.
        const double S0 = 100, K = 100, r = 0.05, q = 0.0, v = 0.35;
        TimeLine fixings;
        for (int i = 1; i <= 12; ++i)
            fixings.push_back(i / 12.0);

        BlackScholesSimModel<Real> m1(S0, r, q, v), m2(S0, r, q, v);
        SimulatableAsian<Real> ari(fixings, K, true, false);
        SimulatableAsian<Real> geo(fixings, K, true, true);
        const auto a = simulate<Real>(ari, m1, mc_settings());
        const auto g = simulate<Real>(geo, m2, mc_settings());
        EXPECT_GT(a.npv(), g.npv() - 4.0 * (a.std_error() + g.std_error()));
    }

    TEST(SimulatableAsian, RejectsNonPositiveFixing)
    {
        EXPECT_THROW(SimulatableAsian<Real>({-0.25, 1.0}, 100.0, true, false),
                     std::invalid_argument);
        EXPECT_THROW(SimulatableAsian<Real>({}, 100.0, true, false),
                     std::invalid_argument);
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

    // ---------------------------------------------------------------------
    // Brownian bridge in the generic engine (blueprint/wp/19-gpu.md §2.4, G1)
    // ---------------------------------------------------------------------

    namespace
    {
        /// Call on the last date of a monitored timeline: the path has many
        /// steps, the payoff only reads the end (exercises the bridge's
        /// increments and the model's non-Brownian draws).
        struct MonitoredCall final : ISimulatableProduct<Real>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            MonitoredCall(Real k, TimeLine t)
                : K(k), tl_(std::move(t)), dl_(tl_.size()) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<Real> &p, std::vector<Real> &out) const override
            {
                out.assign(1, std::max(p.back().spots[0] - K, 0.0) / p.back().numeraire);
            }
        };

        /// Merton (1976) call: Poisson mixture of Black-Scholes prices, each
        /// at rate r_n = r − λk + n ln(1+k)/T and variance σ² + nδ²/T.
        double merton_call(double S, double K, double r, double q, double v, double T,
                           double lambda, double mu, double delta)
        {
            const double k = std::exp(mu + 0.5 * delta * delta) - 1.0;
            const double lp = lambda * (1.0 + k) * T;
            double sum = 0.0, weight = std::exp(-lp);
            for (int n = 0; n < 60; ++n)
            {
                if (n > 0)
                    weight *= lp / n;
                const double rn = r - lambda * k + n * std::log(1.0 + k) / T;
                const double vn = std::sqrt(v * v + n * delta * delta / T);
                sum += weight * bs_call(S, K, rn, q, vn, T); // Hull's form: BS at r_n throughout
            }
            return sum;
        }

        PricingSettings sobol_settings(int paths, bool bridge)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 7;
            s.mc_sampler = SamplerKind::Sobol;
            s.mc_rqmc_batches = 32;
            s.mc_brownian_bridge = bridge;
            return s;
        }

        TimeLine weekly(int n)
        {
            TimeLine t;
            for (int i = 1; i <= n; ++i)
                t.push_back(i / static_cast<double>(n));
            return t;
        }

        /// Least-squares slope of log(se) against log(N).
        double loglog_slope(const std::vector<double> &n, const std::vector<double> &se)
        {
            double mx = 0, my = 0;
            for (std::size_t i = 0; i < n.size(); ++i)
            {
                mx += std::log(n[i]);
                my += std::log(se[i]);
            }
            mx /= static_cast<double>(n.size());
            my /= static_cast<double>(n.size());
            double sxy = 0, sxx = 0;
            for (std::size_t i = 0; i < n.size(); ++i)
            {
                sxy += (std::log(n[i]) - mx) * (std::log(se[i]) - my);
                sxx += (std::log(n[i]) - mx) * (std::log(n[i]) - mx);
            }
            return sxy / sxx;
        }
    } // namespace

    // Pseudo-random N(0,1) in, bridged increments out: W(t_i) rebuilt from
    // them has covariance min(t_i, t_j) (Glasserman 3.1), on an uneven grid.
    TEST(BridgedGaussians, RebuiltPathHasBrownianCovariance)
    {
        const std::vector<Time> t{0.05, 0.1, 0.3, 0.35, 0.8, 1.0, 1.7};
        const std::size_t n = t.size();
        BridgedGaussians bridge(t, 1, 1);
        Pcg32 rng(11, 3);
        std::vector<double> point(n), inc(n);
        std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
        constexpr int paths = 200000;
        for (int p = 0; p < paths; ++p)
        {
            for (auto &x : point)
                x = inverse_normal_cdf(uniform01(rng));
            bridge.map(point, inc);
            std::vector<double> w(n);
            double acc = 0.0, prev = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                acc += inc[i] * std::sqrt(t[i] - prev);
                prev = t[i];
                w[i] = acc;
            }
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                    cov[i][j] += w[i] * w[j];
        }
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
            {
                const double target = std::min(t[i], t[j]);
                // sd of a product-moment estimate <= sqrt(2) t_i t_j / sqrt(N)
                const double tol = 4.0 * std::sqrt(2.0 * t[i] * t[j] / paths);
                EXPECT_NEAR(cov[i][j] / paths, target, tol) << i << "," << j;
            }
    }

    // Non-Brownian draws (jumps) keep their time order after the bridge's
    // coordinates; Brownian coordinate 0 is the terminal value.
    TEST(BridgedGaussians, LayoutOfFactorsAndOtherDraws)
    {
        const std::vector<Time> t{0.25, 0.5, 0.75, 1.0};
        BridgedGaussians bridge(t, 2, 3); // 2 factors + 1 other draw per step
        std::vector<double> point(12, 0.0), out(12);
        point[1] = 1.0; // factor 1's terminal value: W_1(1) = 1
        for (int s = 0; s < 4; ++s)
            point[8 + s] = 10.0 + s; // the other draws, in time order
        bridge.map(point, out);
        for (int s = 0; s < 4; ++s)
        {
            EXPECT_DOUBLE_EQ(out[3 * s + 0], 0.0);                      // factor 0 untouched
            EXPECT_NEAR(out[3 * s + 1], 0.25 / std::sqrt(0.25), 1e-15); // W_1 linear in t
            EXPECT_DOUBLE_EQ(out[3 * s + 2], 10.0 + s);
        }
    }

    TEST(BrownianLayout, ModelsDescribeTheirDraws)
    {
        const TimeLine tl{0.0, 0.25, 0.5, 1.0};
        const std::vector<SampleDef> dl(tl.size());

        BlackScholesSimModel<Real> bs(100.0, 0.05, 0.0, 0.2);
        bs.init(tl, dl);
        EXPECT_TRUE(bs.brownian_layout().covers(bs.sim_dim()));
        EXPECT_EQ(bs.brownian_layout().times, (std::vector<Time>{0.25, 0.5, 1.0})); // t = 0 draws nothing

        MertonJumpDiffusionSimModel<Real> merton(100.0, 0.05, 0.0, 0.2, 0.5, -0.1, 0.15);
        merton.init(tl, dl);
        EXPECT_TRUE(merton.brownian_layout().covers(merton.sim_dim()));
        EXPECT_EQ(merton.brownian_layout().stride, 3u);

        Eigen::MatrixXd corr(2, 2);
        corr << 1.0, 0.5, 0.5, 1.0;
        MultiAssetBSSimModel<Real> basket({100.0, 50.0}, 0.05, {0.0, 0.0}, {0.2, 0.3}, corr);
        basket.init(tl, dl);
        EXPECT_TRUE(basket.brownian_layout().covers(basket.sim_dim()));
        EXPECT_EQ(basket.brownian_layout().factors, 2u);
    }

    // Unbiased: Sobol + bridge on 52 weekly fixings against the closed form,
    // at a standard error around 1e-3 -- a wrong bridge would show at once.
    TEST(BrownianBridgeEngine, GeometricAsianMatchesClosedForm)
    {
        const double S0 = 100, K = 100, r = 0.05, q = 0.0, v = 0.25;
        const TimeLine fixings = weekly(52);
        GeoAsianCall prod(K, fixings);
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, sobol_settings(1 << 16, true));
        EXPECT_NE(res.diagnostics.find("Brownian bridge"), std::string::npos);
        EXPECT_LT(res.std_error(), 3e-3);
        EXPECT_NEAR(res.npv(), geo_asian_call(S0, K, r, q, v, fixings), 4.0 * res.std_error());
    }

    // The jump draws ride along untouched: Merton with 24 monitoring steps
    // against Merton's closed form.
    TEST(BrownianBridgeEngine, MertonCallMatchesClosedForm)
    {
        const double S0 = 100, K = 105, r = 0.03, q = 0.01, v = 0.2, T = 1.0;
        const double lambda = 0.8, mu = -0.1, delta = 0.15;
        MonitoredCall prod(K, weekly(24));
        MertonJumpDiffusionSimModel<Real> model(S0, r, q, v, lambda, mu, delta);
        const auto res = simulate<Real>(prod, model, sobol_settings(1 << 16, true));
        EXPECT_NE(res.diagnostics.find("Brownian bridge"), std::string::npos);
        EXPECT_NEAR(res.npv(), merton_call(S0, K, r, q, v, T, lambda, mu, delta),
                    4.0 * res.std_error());
    }

    // G1's acceptance (blueprint §11): on an arithmetic Asian with 52
    // fixings, Sobol + bridge converges like ~N^-0.9 in log-log, pseudo-random
    // like N^-0.5, and the bridge beats Sobol in plain time order. Fixed
    // seeds: the numbers are deterministic, the margins are wide.
    TEST(BrownianBridgeEngine, SobolWithBridgeConvergesFasterOnAnAsian)
    {
        SimulatableAsian<Real> prod(weekly(52), 100.0, /*is_call=*/true, /*geometric=*/false);
        BlackScholesSimModel<Real> model(100.0, 0.05, 0.0, 0.25);
        std::vector<double> n, se_bridge, se_plain, se_pseudo;
        for (int k = 11; k <= 16; ++k)
        {
            n.push_back(std::ldexp(1.0, k));
            se_bridge.push_back(simulate<Real>(prod, model, sobol_settings(1 << k, true)).std_error());
            se_plain.push_back(simulate<Real>(prod, model, sobol_settings(1 << k, false)).std_error());
            PricingSettings pseudo;
            pseudo.mc_paths = 1 << k;
            pseudo.mc_seed = 7;
            pseudo.mc_antithetic = false;
            pseudo.mc_gaussian = GaussianKind::InverseNormal;
            se_pseudo.push_back(simulate<Real>(prod, model, pseudo).std_error());
        }
        const double slope_bridge = loglog_slope(n, se_bridge);
        const double slope_pseudo = loglog_slope(n, se_pseudo);
        EXPECT_LT(slope_bridge, -0.75);
        EXPECT_GT(slope_pseudo, -0.62);
        EXPECT_LT(slope_pseudo, -0.38);
        EXPECT_LT(se_bridge.back() * 8.0, se_pseudo.back());
        EXPECT_LT(se_bridge.back(), se_plain.back());
    }

} // namespace quantModeling
