#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/market/slv_calibration.hpp"
#include "quantModeling/market/svi_surface.hpp"
#include "quantModeling/models/equity/curve_carry.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/scripting/ir_path.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

// WP 32 F2: the CPU simulation models step on a ForwardCurve's carry and
// discount on its DF(t) (blueprint/wp/32-forward-curve.md).

namespace quantModeling
{
    namespace
    {
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;

        const double S0 = 100.0;
        const double kDay = 1.0 / 365.0;

        // An SPX-like term structure (WP 31 §4.7): the rate rising from 4.4 %
        // to 5.1 %, a steady yield, and a cash dividend of 1.5 on day 91 as
        // two pillars a second apart (WP 32 §2).
        ForwardCurve market_curve()
        {
            const double tau = 91.0 * kDay, gap = kDay / 86400.0;
            const std::vector<Time> T{2.0 / 12, tau, tau + gap, 0.5, 1.0, 2.0};
            const std::vector<Real> r{0.044, 0.045, 0.045, 0.047, 0.050, 0.051};
            std::vector<Real> F, DF;
            for (std::size_t i = 0; i < T.size(); ++i)
            {
                const double fwd = S0 * std::exp((r[i] - 0.012) * T[i]);
                F.push_back(T[i] > tau ? fwd - 1.5 * std::exp((r[i] - 0.012) * (T[i] - tau)) : fwd);
                DF.push_back(std::exp(-r[i] * T[i]));
            }
            return ForwardCurve::from_pillars(S0, T, F, DF);
        }

        ScriptModelSpec spec_of(const std::string &model)
        {
            ScriptModelSpec s;
            s.model = model;
            s.spot = S0;
            s.rate = 0.03;
            s.dividend = 0.01;
            s.vol = 0.2;
            s.max_dt = 1.0 / 52.0;
            s.heston = HestonParams{0.04, 1.5, 0.04, 0.5, -0.7};
            if (model == "local_vol" || model == "slv")
            {
                s.K_grid = {40.0, 80.0, 100.0, 120.0, 250.0};
                s.T_grid = {0.05, 0.5, 3.0};
                for (std::size_t i = 0; i < s.K_grid.size(); ++i)
                    for (std::size_t j = 0; j < s.T_grid.size(); ++j)
                    {
                        // A skewed surface (or leverage): the drift then feeds the vol.
                        const double skew = 0.2 - 0.08 * std::log(s.K_grid[i] / S0);
                        s.sigma_loc_flat.push_back(skew);
                        s.leverage_flat.push_back(skew / 0.2);
                    }
            }
            if (model == "rough_bergomi")
            {
                s.xi_times = {0.25, 3.0};
                s.xi_values = {0.04, 0.045};
                s.max_dt = kDay;
            }
            return s;
        }

        const std::vector<std::string> kModels{"black_scholes", "local_vol", "heston", "slv", "rough_bergomi"};

        // Event dates on rough Bergomi's daily grid, on both sides of the
        // pillars and the dividend; each asks for discounts and forwards,
        // one maturity past the curve's last pillar.
        std::vector<Time> kDates{30 * kDay, 61 * kDay, 91 * kDay, 92 * kDay, 146 * kDay};

        std::vector<SampleDef> defline_of(const std::vector<Time> &dates)
        {
            std::vector<SampleDef> d(dates.size());
            for (std::size_t i = 0; i < dates.size(); ++i)
            {
                d[i].discount_mats = {dates[i] + 0.5, 2.5};
                d[i].forward_mats = {dates[i] + 0.25, 3.0};
            }
            return d;
        }

        struct Paths
        {
            std::unique_ptr<ISimulationModel<Real>> model;
            std::vector<Time> dates;
            std::vector<SampleDef> defline;
            std::mt19937_64 rng{20261127};
            std::normal_distribution<double> normal;
            std::vector<double> z;
            Scenario<Real> path;

            Paths(const ScriptModelSpec &s, std::vector<Time> d, std::uint64_t seed = 20261127)
                : model(make_script_model<Real>(s)), dates(std::move(d)), defline(defline_of(dates)), rng(seed)
            {
                model->init(dates, defline);
                allocate_scenario(path, defline, 1);
                z.resize(model->sim_dim());
            }
            const Scenario<Real> &next()
            {
                for (double &x : z)
                    x = normal(rng);
                model->generate_path(std::span<const double>(z), path);
                return path;
            }
        };

        std::size_t paths_for(const std::string &model)
        {
            return model == "rough_bergomi" ? 8000 : 40000;
        }
    } // namespace

    // A flat curve is its (rate, dividend): every model's path bit for bit,
    // rate and dividend fields then not read.
    TEST(ForwardCurveModels, FlatCurveIsBitForBitTheScalars)
    {
        for (const std::string &name : kModels)
        {
            ScriptModelSpec scalar = spec_of(name), curved = spec_of(name);
            curved.forward_curve = ForwardCurve::flat(S0, scalar.rate, scalar.dividend);
            curved.rate = 0.5;
            curved.dividend = 0.25;
            Paths a(scalar, kDates), b(curved, kDates);
            ASSERT_EQ(a.model->sim_dim(), b.model->sim_dim()) << name;
            for (int p = 0; p < 3; ++p)
            {
                const Scenario<Real> &x = a.next(), &y = b.next();
                for (std::size_t e = 0; e < x.size(); ++e)
                {
                    EXPECT_EQ(x[e].spots[0], y[e].spots[0]) << name << " " << e;
                    EXPECT_EQ(x[e].numeraire, y[e].numeraire) << name << " " << e;
                    for (std::size_t m = 0; m < 2; ++m)
                    {
                        EXPECT_EQ(x[e].discounts[m], y[e].discounts[m]) << name;
                        EXPECT_EQ(x[e].forwards[m], y[e].forwards[m]) << name;
                    }
                }
            }
            // The device keeps a flat curve: nothing changes for the GPU.
            DeviceModel d1, d2;
            EXPECT_EQ(a.model->describe_device(d1), b.model->describe_device(d2)) << name;
        }
    }

    // Zero coupons: the numeraire is 1 / DF(t) on every path, and the
    // discounts and forwards a payoff reads are the curve's.
    TEST(ForwardCurveModels, RatesAndForwardsAreTheCurves)
    {
        const ForwardCurve c = market_curve();
        for (const std::string &name : kModels)
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = c;
            Paths p(s, kDates);
            // F4: the device carries the curve and the model's carry per step
            // (rough Bergomi has no device description).
            DeviceModel d;
            if (name != "rough_bergomi")
            {
                ASSERT_TRUE(p.model->describe_device(d)) << name;
                ASSERT_TRUE(d.curve.has_value()) << name;
                EXPECT_EQ(d.step_carry.size(), d.t.size()) << name;
            }
            const Scenario<Real> &x = p.next();
            for (std::size_t e = 0; e < kDates.size(); ++e)
            {
                const Time t = kDates[e];
                EXPECT_NEAR(x[e].numeraire * c.discount(t), 1.0, 1e-14) << name << " " << e;
                for (std::size_t m = 0; m < 2; ++m)
                {
                    const Time dm = p.defline[e].discount_mats[m], fm = p.defline[e].forward_mats[m];
                    EXPECT_NEAR(x[e].discounts[m] / (c.discount(dm) / c.discount(t)), 1.0, 1e-14) << name;
                    EXPECT_NEAR(x[e].forwards[m] / (x[e].spots[0] * c.forward(fm) / c.forward(t)), 1.0, 1e-14)
                        << name;
                }
            }
        }
    }

    // E[S(t) / N(t)] = F(t) DF(t) under every model: the spot is a
    // martingale after the curve's carry, the dividend's drop included; and
    // the control variates' deflated mean says the same.
    TEST(ForwardCurveModels, DeflatedSpotIsAMartingaleOnTheCurve)
    {
        const ForwardCurve c = market_curve();
        for (const std::string &name : kModels)
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = c;
            Paths p(s, kDates);
            const std::size_t n = paths_for(name);
            std::vector<double> sum(kDates.size(), 0.0), sum2(kDates.size(), 0.0);
            for (std::size_t i = 0; i < n; ++i)
            {
                const Scenario<Real> &x = p.next();
                for (std::size_t e = 0; e < kDates.size(); ++e)
                {
                    const double y = x[e].spots[0] / x[e].numeraire;
                    sum[e] += y;
                    sum2[e] += y * y;
                }
            }
            for (std::size_t e = 0; e < kDates.size(); ++e)
            {
                const double mean = sum[e] / double(n);
                const double se = std::sqrt((sum2[e] / double(n) - mean * mean) / double(n));
                const double target = c.forward(kDates[e]) * c.discount(kDates[e]);
                EXPECT_NEAR(mean, target, 4.0 * se) << name << " at day " << std::lround(kDates[e] / kDay);
                Real declared = 0.0;
                if (p.model->deflated_spot_mean(0, kDates[e], declared))
                {
                    EXPECT_NEAR(declared, target, 1e-12 * target) << name;
                }
            }
            // The dividend is in the paths: the mean drops by about 1.5
            // between days 91 and 92, where a flat yield would move it by
            // less than 0.01.
            const double drop = sum[2] / double(n) - sum[3] / double(n);
            EXPECT_GT(drop, 1.2) << name;
        }
    }

    // A vanilla at a date reprices on the curve: where the drift does not
    // feed the vol (Black-Scholes, Heston, rough Bergomi), the path on the
    // curve is the path under the flat (r, q) fitted to F(T) and DF(T),
    // scaled, so their prices agree to rounding with common draws; and
    // Black-Scholes matches Black's formula on F(T), DF(T).
    TEST(ForwardCurveModels, VanillaAtEachDateIsTheFlatFitAtItsMaturity)
    {
        const ForwardCurve c = market_curve();
        for (const char *name : {"black_scholes", "heston", "rough_bergomi"})
            for (const Time T : {61 * kDay, 92 * kDay, 146 * kDay})
            {
                ScriptModelSpec on_curve = spec_of(name), flat = spec_of(name);
                on_curve.forward_curve = c;
                flat.rate = -std::log(c.discount(T)) / T;
                flat.dividend = flat.rate - std::log(c.forward(T) / S0) / T;
                Paths a(on_curve, {T}), b(flat, {T});
                const double K = c.forward(T) * 1.02;
                double pa = 0.0, pb = 0.0, pa2 = 0.0;
                const std::size_t n = paths_for(name) / 4;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Scenario<Real> &x = a.next(), &y = b.next();
                    const double va = std::max(x[0].spots[0] - K, 0.0) / x[0].numeraire;
                    pa += va;
                    pa2 += va * va;
                    pb += std::max(y[0].spots[0] - K, 0.0) / y[0].numeraire;
                }
                EXPECT_NEAR(pa / pb, 1.0, 1e-11) << name << " T = " << T;
                if (std::string(name) == "black_scholes")
                {
                    const double mean = pa / double(n);
                    const double se = std::sqrt((pa2 / double(n) - mean * mean) / double(n));
                    EXPECT_NEAR(mean, black76_call_price(c.forward(T), K, T, 0.2, c.discount(T)), 4.0 * se);
                }
            }
    }

    // Heston on the curve against its closed form on F(T) and DF(T): the
    // COS price at every date (weekly Euler: a bias well inside 4 s.e.).
    TEST(ForwardCurveModels, HestonVanillaMatchesTheClosedFormOnTheCurve)
    {
        const ForwardCurve c = market_curve();
        ScriptModelSpec s = spec_of("heston");
        s.forward_curve = c;
        s.max_dt = 1.0 / 365.0;
        for (const Time T : {61 * kDay, 146 * kDay})
        {
            Paths p(s, {T});
            const double K = c.forward(T);
            double sum = 0.0, sum2 = 0.0;
            const std::size_t n = 60000;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Scenario<Real> &x = p.next();
                const double v = std::max(x[0].spots[0] - K, 0.0) / x[0].numeraire;
                sum += v;
                sum2 += v * v;
            }
            const double mean = sum / double(n), se = std::sqrt((sum2 / double(n) - mean * mean) / double(n));
            EXPECT_NEAR(mean, heston_cos_price(c.forward(T), K, T, c.discount(T), s.heston, true), 4.0 * se)
                << T;
        }
    }

    // On a curve the model's rate is a parallel shift over it: its pathwise
    // AAD rho is the derivative of the price on curve.shifted(h, 0), what
    // the bumped rho of script_route computes.
    TEST(ForwardCurveModels, RateParameterIsAParallelShiftOfTheCurve)
    {
        using aad::Number;
        const ForwardCurve c = market_curve();
        const std::vector<Time> dates{61 * kDay, 146 * kDay};
        const double K = 101.0;
        for (const char *name : {"black_scholes", "local_vol", "slv"})
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = c;
            std::mt19937_64 rng(7);
            std::normal_distribution<double> normal;
            auto price_on = [&](const ScriptModelSpec &spec, const std::vector<double> &z)
            {
                Paths p(spec, dates);
                p.model->generate_path(std::span<const double>(z), p.path);
                return std::max(p.path[1].spots[0] - K, 0.0) / p.path[1].numeraire +
                       p.path[0].discounts[1] / p.path[0].numeraire;
            };
            Paths probe(s, dates);
            std::vector<double> z(probe.model->sim_dim());
            for (double &x : z)
                x = 0.3 * normal(rng);

            aad::Tape tape;
            aad::Tape *saved = Number::tape;
            Number::tape = &tape;
            auto m = make_script_model<Number>(s);
            m->init(dates, defline_of(dates));
            Scenario<Number> path;
            allocate_scenario(path, defline_of(dates), 1);
            m->generate_path(std::span<const double>(z), path);
            Number v = max(path[1].spots[0] - K, Number(0.0)) / path[1].numeraire + path[0].discounts[1] / path[0].numeraire;
            v.propagate_to_start();
            const std::vector<std::string> &labels = m->parameter_labels();
            const std::size_t ir = static_cast<std::size_t>(std::find(labels.begin(), labels.end(), "rate") - labels.begin());
            const std::size_t iq = static_cast<std::size_t>(std::find(labels.begin(), labels.end(), "div") - labels.begin());
            const double rho = m->parameters()[ir]->adjoint(), div = m->parameters()[iq]->adjoint();
            EXPECT_EQ(m->parameters()[ir]->value(), 0.0) << name << ": the shift is zero on the curve";
            Number::tape = saved;

            const double h = 1e-5;
            ScriptModelSpec up = s, down = s;
            up.forward_curve = c.shifted(h, 0.0);
            down.forward_curve = c.shifted(-h, 0.0);
            EXPECT_NEAR(rho, (price_on(up, z) - price_on(down, z)) / (2 * h), 1e-6 * std::abs(rho) + 1e-7) << name;
            up.forward_curve = c.shifted(0.0, h);
            down.forward_curve = c.shifted(0.0, -h);
            EXPECT_NEAR(div, (price_on(up, z) - price_on(down, z)) / (2 * h), 1e-6 * std::abs(div) + 1e-7) << name;
        }
    }

    // A script priced end to end on the curve (engine, control variates,
    // threads, the GPU declined): a payment of 5 is worth 5 DF(t1), a call
    // Black's price on F(t2) and DF(t2).
    TEST(ForwardCurveModels, ScriptPricesOnTheCurve)
    {
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const ScriptedProduct<Real> product("2026-08-01\n    pays 5\n\n2026-10-21\n    pays max(spot() - 101, 0)\n", ctx);
        const Time t1 = ctx.t(Date::from_iso("2026-08-01")), t2 = ctx.t(Date::from_iso("2026-10-21"));
        const ForwardCurve c = market_curve();
        ScriptModelSpec s = spec_of("black_scholes");
        s.forward_curve = c;
        auto model = make_script_model<Real>(s);
        PricingSettings settings;
        settings.mc_paths = 200000;
        const SimulationMCResult r = simulate_script(product, *model, settings);
        EXPECT_EQ(r.device, "cpu");
        const double expected = 5.0 * c.discount(t1) + black76_call_price(c.forward(t2), 101.0, t2, 0.2, c.discount(t2));
        EXPECT_NEAR(r.values[0], expected, 4.0 * r.std_errors[0] + 1e-12) << r.diagnostics;
        // The flat fit at t2 alone would miss the payment by its discount's error.
        const double flat_r = -std::log(c.discount(t2)) / t2;
        EXPECT_GT(std::abs(5.0 * std::exp(-flat_r * t1) - 5.0 * c.discount(t1)), 4.0 * r.std_errors[0]);
    }

    namespace
    {
        // The curve with one pillar bumped: the zero rate of pillar j by h
        // (DF and F move together, as the scalar rate), or its dividend
        // yield by h (F alone).
        ForwardCurve bumped(const ForwardCurve &c, std::size_t j, bool rate, double h)
        {
            std::vector<Time> T = c.pillar_times();
            std::vector<Real> F, DF = c.discount_curve().pillar_discount_factors();
            for (const Real y : c.pillar_log_forwards())
                F.push_back(c.spot() * std::exp(y));
            if (rate)
            {
                DF[j] *= std::exp(-h * T[j]);
                F[j] *= std::exp(h * T[j]);
            }
            else
                F[j] *= std::exp(-h * T[j]);
            return ForwardCurve::from_pillars(c.spot(), T, F, DF);
        }

        // A payoff that reads every curve quantity: a call, a discount and a
        // forward at the first date, all deflated.
        template <class T>
        T payoff(const Scenario<T> &x)
        {
            using std::max;
            return max(x[1].spots[0] - T(101.0), T(0.0)) / x[1].numeraire + x[0].discounts[1] / x[0].numeraire +
                   0.01 * x[0].forwards[1] / x[0].numeraire;
        }

        std::size_t index_of(const std::vector<std::string> &labels, const std::string &l)
        {
            return static_cast<std::size_t>(std::find(labels.begin(), labels.end(), l) - labels.begin());
        }
    } // namespace

    // WP 32 F3: the pathwise risk to each pillar's rate and dividend yield
    // is the bumped pillar's, and they sum to the scalar rho and dividend
    // risk, under every model.
    TEST(ForwardCurveModels, PillarRisksMatchBumpsAndSumToTheScalars)
    {
        using aad::Number;
        const ForwardCurve c = market_curve();
        const std::size_t n = c.pillar_times().size();
        const std::vector<Time> dates{61 * kDay, 146 * kDay};
        for (const std::string &name : kModels)
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = c;
            Paths probe(s, dates);
            std::mt19937_64 rng(11);
            std::normal_distribution<double> normal;
            std::vector<double> z(probe.model->sim_dim());
            for (double &x : z)
                x = 0.3 * normal(rng);
            auto price_on = [&](const ForwardCurve &curve)
            {
                ScriptModelSpec b = s;
                b.forward_curve = curve;
                Paths p(b, dates);
                p.model->generate_path(std::span<const double>(z), p.path);
                return payoff(p.path);
            };

            aad::Tape tape;
            aad::Tape *saved = Number::tape;
            Number::tape = &tape;
            auto m = make_script_model<Number>(s);
            m->init(dates, defline_of(dates));
            Scenario<Number> path;
            allocate_scenario(path, defline_of(dates), 1);
            m->generate_path(std::span<const double>(z), path);
            Number v = payoff(path);
            v.propagate_to_start();
            const std::vector<std::string> labels = m->parameter_labels();
            std::vector<double> adj;
            for (Number *x : m->parameters())
                adj.push_back(x->adjoint());
            Number::tape = saved;

            ASSERT_EQ(labels.size(), adj.size());
            double rate_sum = 0.0, div_sum = 0.0;
            const double h = 1e-6;
            for (std::size_t j = 0; j < n; ++j)
            {
                const std::string jj = std::to_string(j);
                const std::size_t ir = index_of(labels, CurveCarry::rate_label(c.pillar_times()[j]));
                const std::size_t iq = index_of(labels, CurveCarry::dividend_label(c.pillar_times()[j]));
                ASSERT_LT(ir, labels.size()) << name << " rate" << jj;
                ASSERT_LT(iq, labels.size()) << name << " div" << jj;
                rate_sum += adj[ir];
                div_sum += adj[iq];
                const double fd_r = (price_on(bumped(c, j, true, h)) - price_on(bumped(c, j, true, -h))) / (2 * h);
                const double fd_q = (price_on(bumped(c, j, false, h)) - price_on(bumped(c, j, false, -h))) / (2 * h);
                EXPECT_NEAR(adj[ir], fd_r, 1e-6 * std::abs(fd_r) + 1e-7) << name << " rate" << jj;
                EXPECT_NEAR(adj[iq], fd_q, 1e-6 * std::abs(fd_q) + 1e-7) << name << " div" << jj;
            }
            const double rho = adj[index_of(labels, "rate")], div = adj[index_of(labels, "div")];
            EXPECT_NE(rho, 0.0) << name;
            EXPECT_NEAR(rate_sum, rho, 1e-10 * std::abs(rho)) << name;
            EXPECT_NEAR(div_sum, div, 1e-10 * std::abs(div)) << name;
        }
    }

    // The same through the adjoint engine on a script: every pillar is a
    // risk with its standard error, and the pillars sum to the scalars.
    TEST(ForwardCurveModels, ScriptAdjointRisksPerPillar)
    {
        using aad::Number;
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const std::string src = "2026-08-01\n    pays 5\n\n2026-10-21\n    pays max(spot() - 101, 0)\n";
        const ForwardCurve c = market_curve();
        for (const char *name : {"black_scholes", "local_vol"})
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = c;
            const ScriptedProduct<Number> product(src, ctx);
            auto model = make_script_model<Number>(s);
            const AADSimulResults r = simulate_aad(product, *model, 20000, 3);
            const std::size_t n = c.pillar_times().size();
            double rate_sum = 0.0, div_sum = 0.0;
            for (std::size_t j = 0; j < n; ++j)
            {
                const std::size_t ir = index_of(r.risk_labels, CurveCarry::rate_label(c.pillar_times()[j]));
                const std::size_t iq = index_of(r.risk_labels, CurveCarry::dividend_label(c.pillar_times()[j]));
                ASSERT_LT(ir, r.risks.size()) << name;
                ASSERT_LT(iq, r.risks.size()) << name;
                rate_sum += r.risks[ir];
                div_sum += r.risks[iq];
            }
            const double rho = r.risks[index_of(r.risk_labels, "rate")];
            const double div = r.risks[index_of(r.risk_labels, "div")];
            EXPECT_NEAR(rate_sum, rho, 1e-9 * std::abs(rho)) << name;
            EXPECT_NEAR(div_sum, div, 1e-9 * std::abs(div)) << name;
            // The payment of 5 on day 61 reads the discount pillars around
            // it only: no risk to the pillars past the call's date.
            const std::size_t last = index_of(r.risk_labels, CurveCarry::rate_label(c.pillar_times()[n - 1]));
            EXPECT_EQ(r.risks[last], 0.0) << name;

            // On threads, each with its clone: the pillar shifts are leaves
            // of every clone's tape, the same bits as on one thread.
            ThreadPool pool;
            pool.start_on_demand(3);
            auto m1 = make_script_model<Number>(s), m2 = make_script_model<Number>(s);
            const AADSimulResults one = simulate_aad(product, *m1, 4096, 5, first_aad_payoff, RngKind::Philox,
                                                     SamplerKind::PseudoRandom, nullptr, nullptr);
            const AADSimulResults many = simulate_aad(product, *m2, 4096, 5, first_aad_payoff, RngKind::Philox,
                                                      SamplerKind::PseudoRandom, nullptr, &pool);
            ASSERT_EQ(one.risks.size(), many.risks.size());
            for (std::size_t i = 0; i < one.risks.size(); ++i)
                EXPECT_EQ(one.risks[i], many.risks[i]) << name << " " << one.risk_labels[i];
        }
    }

    // WP 32 F5: the Dupire grid places its strikes at the curve's forwards,
    // and SLV's particles step on its carry; a flat curve is the scalars'
    // grid and leverage, bit for bit.
    TEST(ForwardCurveModels, DupireGridAndLeverageOnTheCurve)
    {
        std::vector<SVISliceCalibration> slices;
        for (const double T : {0.25, 0.5, 1.0, 2.0})
        {
            SVISliceCalibration c;
            c.ttm = T;
            c.params = SVIParams{0.01 * T, 0.08 * T, -0.4, 0.0, 0.2};
            slices.push_back(c);
        }
        const SVISurface surface(slices);
        const double r = 0.04, q = 0.01;
        const ForwardCurve flat = ForwardCurve::flat(S0, r, q);
        const GridLocalVol plain = build_local_vol_grid(surface, S0, r, q, -0.5, 0.5, 30, 12);
        const GridLocalVol same = build_local_vol_grid(surface, S0, r, q, -0.5, 0.5, 30, 12, {}, nullptr, &flat);
        EXPECT_EQ(plain.K_grid(), same.K_grid());
        EXPECT_EQ(plain.sigma_loc(), same.sigma_loc());

        // On a curve: the strikes span the band at its forward at the mid
        // maturity, and the scalars are not read.
        const ForwardCurve c = market_curve();
        const GridLocalVol on = build_local_vol_grid(surface, S0, 0.5, 0.5, -0.5, 0.5, 30, 12, {}, nullptr, &c);
        EXPECT_NEAR(on.K_grid().front(), c.forward(0.5 * (0.25 + 2.0)) * std::exp(-0.5), 1e-12);
        const GridLocalVol on2 = build_local_vol_grid(surface, S0, 0.0, 0.0, -0.5, 0.5, 30, 12, {}, nullptr, &c);
        EXPECT_EQ(on.sigma_loc(), on2.sigma_loc());

        SLVCalibrationSettings settings;
        settings.n_particles = 4000;
        const HestonParams h{0.04, 1.5, 0.04, 0.5, -0.7};
        const SLVLeverageGrid a =
            calibrate_slv_leverage(S0, r, q, h, plain.K_grid(), plain.T_grid(), plain.sigma_loc(), settings);
        const SLVLeverageGrid b = calibrate_slv_leverage(S0, r, q, h, plain.K_grid(), plain.T_grid(),
                                                         plain.sigma_loc(), settings, &flat);
        EXPECT_EQ(a.leverage, b.leverage);
        const SLVLeverageGrid d1 =
            calibrate_slv_leverage(S0, 0.5, 0.5, h, on.K_grid(), on.T_grid(), on.sigma_loc(), settings, &c);
        const SLVLeverageGrid d2 =
            calibrate_slv_leverage(S0, 0.0, 0.0, h, on.K_grid(), on.T_grid(), on.sigma_loc(), settings, &c);
        EXPECT_EQ(d1.leverage, d2.leverage);
        EXPECT_NE(d1.leverage, a.leverage);
    }

    // WP 32 F4: the IR's adjoint on the CPU (what the GPU's kernels are
    // generated from) on a curve, against the tape on the same Philox
    // paths: the model's own risks, the rate and dividend as parallel
    // shifts, and the pillars the tape's only.
    TEST(ForwardCurveModels, TheIrAdjointOnTheCurveIsTheTapes)
    {
        using aad::Number;
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const std::string src = "2026-08-01\n    s1 = spot()\n    pays 0.1 * df(2028-12-01)\n\n"
                                "2027-03-01\n    pays max(spot() - s1, 0)\n    pays 0.5 * fwd(2027-09-01)\n";
        for (const char *name : {"black_scholes", "local_vol", "heston", "slv"})
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = market_curve();
            const ScriptedProduct<Real> product(src, ctx);
            auto model = make_script_model<Real>(s);
            std::string why;
            const auto ir = simulate_script_aad_ir(product, *model, 2048, 5, why);
            ASSERT_TRUE(ir.has_value()) << name << ": " << why;
            const ScriptedProduct<Number> tape_product(src, ctx);
            auto tape_model = make_script_model<Number>(s);
            const AADSimulResults tape =
                simulate_aad(tape_product, *tape_model, 2048, 5, first_aad_payoff, RngKind::Philox);
            EXPECT_NEAR(ir->price, tape.price, 1e-12 * std::abs(tape.price)) << name;
            double largest = 0.0;
            for (const double x : tape.risks)
                largest = std::max(largest, std::abs(x));
            // every label, the pillars' among them (WP 32)
            ASSERT_EQ(ir->risk_labels.size(), tape.risk_labels.size()) << name;
            for (std::size_t i = 0; i < ir->risk_labels.size(); ++i)
            {
                ASSERT_EQ(ir->risk_labels[i], tape.risk_labels[i]) << name;
                EXPECT_NEAR(ir->risks[i], tape.risks[i], 1e-10 * largest) << name << " " << tape.risk_labels[i];
            }
        }
    }

    // WP 32: the pillar shifts in the IR (CurveShift) -- the tangent in
    // every direction, the pillars' among them, is the adjoint's gradient
    // path by path, under every model of the IR; the paths' bits are those
    // of the program without pillars.
    TEST(ForwardCurveModels, TheIrPillarTangentIsTheAdjoint)
    {
        namespace ir = scripting::ir;
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const std::string src = "2026-08-01\n    s1 = spot()\n    pays 0.1 * df(2028-12-01)\n\n"
                                "2027-03-01\n    pays max(spot() - s1, 0)\n    pays 0.5 * fwd(2027-09-01)\n";
        for (const char *name : {"black_scholes", "local_vol", "heston", "slv"})
        {
            ScriptModelSpec s = spec_of(name);
            s.forward_curve = market_curve();
            const ScriptedProduct<Real> product(src, ctx);
            auto model = make_script_model<Real>(s);
            model->init(product.timeline(), product.defline());
            DeviceModel dm;
            ASSERT_TRUE(model->describe_device(dm)) << name;
            std::vector<std::vector<Time>> mats;
            for (const SampleDef &def : product.defline())
                mats.push_back(def.discount_mats);
            const int P = static_cast<int>(CurveCarry(*dm.curve).n_shifts());
            const ir::PathProgram plain = ir::path_program(product.ir(), dm.kind, 1, true, false, false, false, false, true);
            const ir::PathProgram primal =
                ir::path_program(product.ir(), dm.kind, 1, true, false, false, false, false, true, P);
            ASSERT_EQ(primal.layout.n_risks, plain.layout.n_risks + P) << name;
            std::vector<std::int32_t> dirs(static_cast<std::size_t>(primal.layout.n_risks));
            std::iota(dirs.begin(), dirs.end(), 0);
            const ir::PathProgram tan = ir::tangent(primal, dirs);
            const ir::PathProgram adj = ir::adjoint(primal);
            const ir::PathData data = ir::path_data(primal.layout, dm, product.ir(), mats, product.baseline());
            const ir::PathData plain_data = ir::path_data(plain.layout, dm, product.ir(), mats, product.baseline());
            ir::DerivativeScratch w;
            ir::PathScratch plain_scratch(plain);
            mc::PathDraws draws;
            draws.seed = 11;
            draws.stride = dm.stride;
            const std::size_t K = dirs.size();
            double largest_pillar = 0.0;
            for (int p = 0; p < 16; ++p)
            {
                std::vector<double> forward(K), gradient(data.theta.size(), 0.0);
                draws.begin(static_cast<uint64_t>(p));
                const double a =
                    ir::run_path_tangent(tan, static_cast<int>(K), data, dm, product.ir(), draws, 1.0, w, forward.data());
                draws.begin(static_cast<uint64_t>(p));
                const double b = ir::run_path_adjoint(primal, adj, data, dm, product.ir(), draws, 1.0, 1.0, w,
                                                      gradient.data());
                draws.begin(static_cast<uint64_t>(p));
                const double c = ir::run_path(plain, plain_data, dm, product.ir(), draws, 1.0, plain_scratch);
                EXPECT_EQ(a, b) << name;
                EXPECT_EQ(a, c) << name; // the shifts at zero leave the path's bits
                double scale = 0.0;
                for (std::size_t k = 0; k < K; ++k)
                    scale = std::max(scale, std::abs(gradient[k]));
                for (std::size_t k = 0; k < K; ++k)
                    EXPECT_NEAR(forward[k], gradient[k], 1e-11 * (1.0 + scale)) << name << " risk " << k;
                for (int j = 0; j < P; ++j)
                    largest_pillar =
                        std::max(largest_pillar, std::abs(gradient[static_cast<std::size_t>(primal.layout.th_pillar0 + j)]));
            }
            EXPECT_GT(largest_pillar, 0.0) << name;
        }
    }

    TEST(ForwardCurveModels, ModelsWithoutACurveRefuseIt)
    {
        ScriptModelSpec s = spec_of("black_scholes");
        s.forward_curve = market_curve();
        s.spots = {100.0, 50.0};
        s.dividends = {0.0, 0.0};
        s.vols = {0.2, 0.3};
        s.correlation = {1.0, 0.5, 0.5, 1.0};
        EXPECT_THROW(make_script_model<Real>(s), InvalidInput);
        ScriptModelSpec hw = spec_of("hull_white");
        hw.forward_curve = market_curve();
        EXPECT_THROW(make_script_model<Real>(hw), InvalidInput);
    }

    // The shifted curve: exact parallel shifts on and off the pillars.
    TEST(ForwardCurve, ShiftedIsAParallelShift)
    {
        const ForwardCurve c = market_curve(), up = c.shifted(0.01, 0.003);
        for (const Time t : {0.01, 0.2, 0.7, 1.5, 4.0})
        {
            EXPECT_NEAR(up.discount(t) / c.discount(t), std::exp(-0.01 * t), 1e-14) << t;
            EXPECT_NEAR(up.forward(t) / c.forward(t), std::exp(0.007 * t), 1e-14) << t;
        }
        const ForwardCurve f = ForwardCurve::flat(S0, 0.04, 0.01).shifted(0.01, 0.003);
        EXPECT_TRUE(f.is_flat());
        EXPECT_DOUBLE_EQ(f.flat_rate(), 0.05);
        EXPECT_DOUBLE_EQ(f.flat_dividend(), 0.013);
    }

} // namespace quantModeling
