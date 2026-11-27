#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
            DeviceModel d;
            EXPECT_FALSE(p.model->describe_device(d)) << name << ": the device has a flat rate until F4";
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
