#include "quantModeling/engines/mc/short_rate.hpp"

#include "quantModeling/models/rates/short_rate_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────────
    //  Helpers
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {

        constexpr int STEPS_PER_YEAR = 252;

        struct MCParams
        {
            std::size_t n_paths;
            uint64_t seed;

            explicit MCParams(const PricingSettings &s)
                : n_paths(s.mc_paths > 0
                              ? static_cast<std::size_t>(s.mc_paths)
                              : std::size_t{100000}),
                  seed(s.mc_seed > 0
                           ? static_cast<uint64_t>(s.mc_seed)
                           : uint64_t{42}) {}
        };

        /**
         * @brief Flat, eval-major simulation result buffer.
         *
         * Layout: data[eval * n_paths + path]
         *
         * Eval-major makes the reduction phase (summing over paths for a
         * given eval time) iterate over contiguous memory, which is
         * cache- and auto-vectorisation-friendly.  The simulation phase
         * writes a few scattered slots per path (one per eval time),
         * which is fine because n_evals is tiny compared to n_steps.
         */
        struct SimResult
        {
            std::size_t n_paths;
            std::size_t n_evals;
            std::vector<Real> disc_factors; // flat [n_evals × n_paths]
            std::vector<Real> r_at_times;   // flat [n_evals × n_paths]

            SimResult(std::size_t np, std::size_t ne)
                : n_paths(np), n_evals(ne),
                  disc_factors(np * ne, 1.0),
                  r_at_times(np * ne) {}

            Real &df(std::size_t eval, std::size_t path) noexcept
            {
                return disc_factors[eval * n_paths + path];
            }
            Real df(std::size_t eval, std::size_t path) const noexcept
            {
                return disc_factors[eval * n_paths + path];
            }
            Real &r(std::size_t eval, std::size_t path) noexcept
            {
                return r_at_times[eval * n_paths + path];
            }
            Real r(std::size_t eval, std::size_t path) const noexcept
            {
                return r_at_times[eval * n_paths + path];
            }
        };

        /**
         * @brief Simulate short-rate paths up to horizon T and compute
         *        discount factors D_i = exp(-∫₀^{T_i} r ds) at requested
         *        eval times.
         *
         * Uses flat eval-major buffers (two allocations total, regardless
         * of n_paths) and precomputed integer step triggers to avoid
         * floating-point comparisons in the inner loop.
         */
        SimResult simulate_paths(const IShortRateModel &model,
                                 Real horizon,
                                 const std::vector<Real> &eval_times,
                                 std::size_t n_paths,
                                 uint64_t seed)
        {
            const auto n_evals = eval_times.size();
            const int n_steps = std::max(1, static_cast<int>(
                                                std::round(horizon * STEPS_PER_YEAR)));
            const Real dt = horizon / static_cast<Real>(n_steps);
            const Real sqrt_dt = std::sqrt(dt);

            // Precompute which simulation step triggers each eval time.
            // After step s (0-indexed), t = (s+1)·dt.
            // Trigger when (s+1)·dt ≥ eval_time  ⟹  s ≥ ceil(eval/dt) − 1.
            std::vector<int> trigger_step(n_evals);
            for (std::size_t e = 0; e < n_evals; ++e)
            {
                int s = static_cast<int>(std::ceil(eval_times[e] / dt - 1e-12)) - 1;
                trigger_step[e] = std::clamp(s, 0, n_steps - 1);
            }

            SimResult res(n_paths, n_evals);
            const Real r0 = model.r0();

            // Initialise r_at_times to r0 (disc_factors already 1.0)
            std::fill(res.r_at_times.begin(), res.r_at_times.end(), r0);

            RngFactory rng_fact(seed);

            for (std::size_t p = 0; p < n_paths; ++p)
            {
                Pcg32 rng = rng_fact.make(p);
                NormalBoxMuller normal;

                Real r = r0;
                Real integral_r = 0.0;
                Real t = 0.0;
                std::size_t eval_idx = 0;

                for (int s = 0; s < n_steps; ++s)
                {
                    const Real dW = sqrt_dt * normal(rng);
                    const Real r_old = r;
                    r = model.euler_step(r, t, dt, dW);
                    t += dt;
                    integral_r += 0.5 * (r_old + r) * dt;

                    // Record at precomputed trigger steps (integer compare)
                    while (eval_idx < n_evals && s >= trigger_step[eval_idx])
                    {
                        res.df(eval_idx, p) = std::exp(-integral_r);
                        res.r(eval_idx, p) = r;
                        ++eval_idx;
                    }
                }

                // Fill any remaining eval times at horizon
                while (eval_idx < n_evals)
                {
                    res.df(eval_idx, p) = std::exp(-integral_r);
                    res.r(eval_idx, p) = r;
                    ++eval_idx;
                }
            }

            return res;
        }

        /// Accumulator for on-line mean + variance (single-pass).
        struct MeanVar
        {
            Real sum = 0.0;
            Real sum2 = 0.0;

            void add(Real x) noexcept
            {
                sum += x;
                sum2 += x * x;
            }
            Real mean(std::size_t n) const noexcept
            {
                return sum / static_cast<Real>(n);
            }
            Real std_error(std::size_t n) const noexcept
            {
                const Real m = mean(n);
                const Real var = sum2 / static_cast<Real>(n) - m * m;
                return std::sqrt(var / static_cast<Real>(n));
            }
        };

    } // anonymous namespace

    // ── ZeroCouponBond ───────────────────────────────────────────────────────
    //
    //  P(0,T) ≈ E[exp(-∫₀ᵀ r(s) ds)]

    void ShortRateMCEngine::visit(const ZeroCouponBond &bond)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateMCEngine");
        MCParams mc(ctx_.settings);

        if (bond.maturity <= 0.0)
            throw InvalidInput("ShortRateMCEngine: ZCB maturity must be > 0");

        auto sim = simulate_paths(m, bond.maturity, {bond.maturity},
                                  mc.n_paths, mc.seed);

        // Contiguous reduction over disc_factors[0 * n_paths .. n_paths)
        MeanVar acc;
        const Real *df = sim.disc_factors.data();
        for (std::size_t p = 0; p < mc.n_paths; ++p)
            acc.add(df[p]);

        PricingResult out;
        out.npv = bond.notional * acc.mean(mc.n_paths);
        out.mc_std_error = bond.notional * acc.std_error(mc.n_paths);
        out.diagnostics = "ShortRateMCEngine ZCB (" + m.model_name() +
                          ", paths=" + std::to_string(mc.n_paths) + ")";
        res_ = out;
    }

    // ── FixedRateBond ────────────────────────────────────────────────────────

    void ShortRateMCEngine::visit(const FixedRateBond &bond)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateMCEngine");
        MCParams mc(ctx_.settings);

        if (bond.maturity <= 0.0)
            throw InvalidInput("ShortRateMCEngine: FixedRateBond maturity must be > 0");

        const int freq = bond.coupon_frequency;
        const auto n = static_cast<std::size_t>(
            std::max(1, static_cast<int>(std::round(
                            bond.maturity * static_cast<Real>(freq)))));
        const Real dt_coupon = bond.maturity / static_cast<Real>(n);
        const Real coupon = bond.notional * bond.coupon_rate * dt_coupon;

        std::vector<Real> eval_times;
        eval_times.reserve(n);
        for (std::size_t i = 1; i <= n; ++i)
            eval_times.push_back(dt_coupon * static_cast<Real>(i));

        auto sim = simulate_paths(m, bond.maturity, eval_times,
                                  mc.n_paths, mc.seed);

        MeanVar acc;
        for (std::size_t p = 0; p < mc.n_paths; ++p)
        {
            Real pv = 0.0;
            for (std::size_t i = 0; i < n; ++i)
                pv += coupon * sim.df(i, p);
            pv += bond.notional * sim.df(n - 1, p); // principal
            acc.add(pv);
        }

        PricingResult out;
        out.npv = acc.mean(mc.n_paths);
        out.mc_std_error = acc.std_error(mc.n_paths);
        out.diagnostics = "ShortRateMCEngine FixedRateBond (" + m.model_name() +
                          ", paths=" + std::to_string(mc.n_paths) + ")";
        res_ = out;
    }

    // ── BondOption ───────────────────────────────────────────────────────────
    //
    //  Simulate r to T_option, use analytic P(T_option, T_bond, r(T_option)),
    //  compute payoff, discount back.

    void ShortRateMCEngine::visit(const BondOption &opt)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateMCEngine");
        MCParams mc(ctx_.settings);

        if (opt.option_maturity <= 0.0)
            throw InvalidInput("ShortRateMCEngine: BondOption option_maturity must be > 0");
        if (opt.bond_maturity <= opt.option_maturity)
            throw InvalidInput("ShortRateMCEngine: bond_maturity must be > option_maturity");

        auto sim = simulate_paths(m, opt.option_maturity, {opt.option_maturity},
                                  mc.n_paths, mc.seed);

        MeanVar acc;
        const Real *df = sim.disc_factors.data();
        const Real *rt = sim.r_at_times.data();
        for (std::size_t p = 0; p < mc.n_paths; ++p)
        {
            const Real P_T_S = m.zcb_price(opt.option_maturity,
                                           opt.bond_maturity, rt[p]);
            const Real payoff = opt.is_call
                                    ? std::max(P_T_S - opt.strike, 0.0)
                                    : std::max(opt.strike - P_T_S, 0.0);
            acc.add(payoff * df[p]);
        }

        PricingResult out;
        out.npv = opt.notional * acc.mean(mc.n_paths);
        out.mc_std_error = opt.notional * acc.std_error(mc.n_paths);
        out.diagnostics = "ShortRateMCEngine BondOption (" + m.model_name() +
                          ", paths=" + std::to_string(mc.n_paths) + ")";
        res_ = out;
    }

    // ── CapFloor ─────────────────────────────────────────────────────────────
    //
    //  Each caplet on [T_i, T_{i+1}]:
    //    factor = (1 + K δ),  K_p = 1 / factor
    //    put payoff at T_i = max(K_p − P(T_i,T_{i+1}), 0) × factor
    //
    //  Single simulation to last reset date; record r and DF at each reset.

    void ShortRateMCEngine::visit(const CapFloor &cf)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateMCEngine");
        MCParams mc(ctx_.settings);

        if (cf.schedule.size() < 2)
            throw InvalidInput("ShortRateMCEngine: CapFloor schedule needs ≥ 2 dates");

        const auto n_caplets = cf.schedule.size() - 1;

        // Only keep non-expired reset dates
        std::vector<Real> eval_times;
        std::vector<std::size_t> active_indices;
        for (std::size_t i = 0; i < n_caplets; ++i)
        {
            if (cf.schedule[i] > 0.0)
            {
                eval_times.push_back(cf.schedule[i]);
                active_indices.push_back(i);
            }
        }

        if (eval_times.empty())
        {
            PricingResult out;
            out.npv = 0.0;
            out.mc_std_error = 0.0;
            out.diagnostics = "ShortRateMCEngine CapFloor — all caplets expired";
            res_ = out;
            return;
        }

        // Precompute per-caplet constants (outside path loop)
        struct CapletSpec
        {
            Real Ti, Ti1, factor, K_p;
        };
        std::vector<CapletSpec> specs;
        specs.reserve(active_indices.size());
        for (auto i : active_indices)
        {
            const Real Ti = cf.schedule[i];
            const Real Ti1 = cf.schedule[i + 1];
            const Real delta = Ti1 - Ti;
            const Real factor = 1.0 + cf.strike * delta;
            specs.push_back({Ti, Ti1, factor, 1.0 / factor});
        }

        auto sim = simulate_paths(m, eval_times.back(), eval_times,
                                  mc.n_paths, mc.seed);

        MeanVar acc;
        const auto n_active = active_indices.size();

        for (std::size_t p = 0; p < mc.n_paths; ++p)
        {
            Real path_pv = 0.0;
            for (std::size_t ai = 0; ai < n_active; ++ai)
            {
                const auto &sp = specs[ai];
                const Real r_Ti = sim.r(ai, p);
                const Real df_Ti = sim.df(ai, p);
                const Real P = m.zcb_price(sp.Ti, sp.Ti1, r_Ti);

                const Real payoff = cf.is_cap
                                        ? std::max(sp.K_p - P, 0.0) * sp.factor
                                        : std::max(P - sp.K_p, 0.0) * sp.factor;
                path_pv += payoff * df_Ti;
            }
            acc.add(path_pv);
        }

        PricingResult out;
        out.npv = cf.notional * acc.mean(mc.n_paths);
        out.mc_std_error = cf.notional * acc.std_error(mc.n_paths);
        out.diagnostics = "ShortRateMCEngine CapFloor (" + m.model_name() +
                          ", paths=" + std::to_string(mc.n_paths) + ")";
        res_ = out;
    }

    // ── Caplet ───────────────────────────────────────────────────────────────
    //
    //  Standalone caplet/floorlet via MC.
    //  Simulate to T_start, compute P(T_start, T_end, r(T_start)) analytically,
    //  apply caplet payoff, discount back.

    void ShortRateMCEngine::visit(const Caplet &cap)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateMCEngine");
        MCParams mc(ctx_.settings);

        if (cap.start <= 0.0)
            throw InvalidInput("ShortRateMCEngine: Caplet start must be > 0");
        if (cap.end <= cap.start)
            throw InvalidInput("ShortRateMCEngine: Caplet end must be > start");

        const Real delta = cap.end - cap.start;
        const Real factor = 1.0 + cap.strike * delta;
        const Real K_p = 1.0 / factor;

        auto sim = simulate_paths(m, cap.start, {cap.start},
                                  mc.n_paths, mc.seed);

        MeanVar acc;
        const Real *df = sim.disc_factors.data();
        const Real *rt = sim.r_at_times.data();
        for (std::size_t p = 0; p < mc.n_paths; ++p)
        {
            const Real P = m.zcb_price(cap.start, cap.end, rt[p]);
            const Real payoff = cap.is_cap
                                    ? std::max(K_p - P, 0.0) * factor  // put on ZCB
                                    : std::max(P - K_p, 0.0) * factor; // call on ZCB
            acc.add(payoff * df[p]);
        }

        PricingResult out;
        out.npv = cap.notional * acc.mean(mc.n_paths);
        out.mc_std_error = cap.notional * acc.std_error(mc.n_paths);
        out.diagnostics = "ShortRateMCEngine Caplet (" + m.model_name() +
                          ", paths=" + std::to_string(mc.n_paths) + ")";
        res_ = out;
    }

} // namespace quantModeling
