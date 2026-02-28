#include "quantModeling/engines/mc/barrier.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/models/volatility.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{

    // ─── validation ────────────────────────────────────────────────────────────

    void BSEuroBarrierMCEngine::validate(const BarrierOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("BarrierOption: payoff is null");
        if (!opt.exercise || opt.exercise->dates().empty())
            throw InvalidInput("BarrierOption: exercise is null or has no dates");
        if (opt.barrier <= 0.0)
            throw InvalidInput("BarrierOption: barrier must be > 0");
        if (opt.notional == 0.0)
            throw InvalidInput("BarrierOption: notional must be non-zero");
    }

    // ─── pricing ───────────────────────────────────────────────────────────────

    void BSEuroBarrierMCEngine::visit(const BarrierOption &opt)
    {
        validate(opt);
        const auto &m = require_model<ILocalVolModel>("BSEuroBarrierMCEngine");
        const PricingSettings settings = ctx_.settings;

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();

        const Real T = opt.exercise->dates().front();
        const Real H = opt.barrier;
        const Real K = opt.payoff->strike();
        const OptionType optype = opt.payoff->type();

        const bool is_up = (opt.barrier_type == BarrierType::UpAndIn ||
                            opt.barrier_type == BarrierType::UpAndOut);
        const bool is_in = (opt.barrier_type == BarrierType::UpAndIn ||
                            opt.barrier_type == BarrierType::DownAndIn);

        // Monitoring steps: 0 → weekly (52/yr) by default.
        // Daily (252/yr) is more accurate but the 9-variant CRN loop is 9× heavier
        // than vanilla MC; weekly keeps latency well inside the 20-second API timeout
        // while still resolving the barrier reasonably.  Pass n_steps > 0 to override.
        const int n_steps = (opt.n_steps > 0)
                                ? opt.n_steps
                                : std::max(1, static_cast<int>(T * 52.0 + 0.5));

        // Two independent RNG streams:
        //   stream 0 → Gaussian draws (GBM increments)
        //   stream 1 → uniform draws  (Brownian-bridge crossing test)
        RngFactory rngFact(settings.mc_seed);
        Pcg32 rng_gauss = rngFact.make(0);
        Pcg32 rng_bb = rngFact.make(1);
        NormalBoxMuller gauss;

        // Central FD bump sizes
        const GreeksBumps bumps;
        const Real dS = S0 * bumps.delta_bump;
        const Real dv = bumps.vega_bump;
        const Real dT = bumps.theta_bump;
        const Real dr = bumps.rho_bump;
        const Real sig_u = sigma + dv;
        const Real sig_d = std::max(1e-8, sigma - dv);
        const Real r_up = r + dr;
        const Real r_dn = r - dr;
        const Real T_up = T + dT;
        const Real T_dn = std::max(1e-6, T - dT);

        // Pre-allocated arrays — drawn once per path, reused for all FD variants.
        // This is the Common Random Numbers (CRN) approach: all variants see the
        // same sequence of random drivers, maximising variance reduction.
        std::vector<Real> zs(n_steps); // Gaussian draws
        std::vector<Real> us(n_steps); // uniform [0,1] for BB correction

        // ── sim_one ─────────────────────────────────────────────────────────────
        // Simulates one complete path (ALL n_steps, no early exit) with the
        // given parameters, using the pre-drawn zs[] and us[] arrays.
        // The BB correction uses the same us[] for all variants, preserving CRN.
        auto sim_one = [&](Real S_start, const IVolatility &vol_v, Real r_val, Real T_val) -> Real
        {
            const Real dt_v = T_val / static_cast<Real>(n_steps);
            const Real sqdt_v = std::sqrt(dt_v);
            const Real df = std::exp(-r_val * T_val);

            Real S = S_start;
            bool hit = false;

            for (int j = 0; j < n_steps; ++j)
            {
                const Real t_cur = static_cast<Real>(j) * dt_v;
                const Real sig = vol_v.value(S, t_cur);
                const Real drift = (r_val - q - 0.5 * sig * sig) * dt_v;
                const Real voldt = sig * sqdt_v;

                const Real S_prev = S;
                S *= std::exp(drift + voldt * zs[j]);

                if (!hit)
                {
                    // ── discrete barrier check ────────────────────────────────
                    if (is_up ? (S >= H) : (S <= H))
                    {
                        hit = true;
                    }
                    // ── Brownian-bridge correction for continuous monitoring ──
                    // P(continuous path crosses H | S_prev, S, no discrete cross)
                    // = exp( -2 ln(H/S_prev) ln(H/S) / (σ² Δt) )
                    // Valid only when both endpoints are on the same side of H,
                    // which is guaranteed since we already checked discrete crossing.
                    else if (opt.brownian_bridge)
                    {
                        const Real log_Ha = std::log(H / S_prev);
                        const Real log_Hb = std::log(H / S);
                        const Real exponent = -2.0 * log_Ha * log_Hb / (sig * sig * dt_v);
                        // exponent < 0 when log_Ha and log_Hb same sign (same side of H)
                        if (exponent < 0.0 && us[j] < std::exp(exponent))
                            hit = true;
                    }
                }
            }

            // Terminal payoff (S is now ST)
            const Real raw_pv = (optype == OptionType::Call)
                                    ? std::max(S - K, 0.0) * opt.notional * df
                                    : std::max(K - S, 0.0) * opt.notional * df;
            const Real reb_pv = opt.rebate * opt.notional * df;

            // Apply knock-in / knock-out logic
            return is_in ? (hit ? raw_pv : reb_pv)
                         : (hit ? reb_pv : raw_pv);
        };

        // FlatVol objects for the 9 CRN variants.
        // FlatVol::value(S, t) is a constant lookup that gives identical
        // numerics to the old scalar-sigma path for BlackScholesModel.
        const FlatVol vol_base(sigma);
        const FlatVol vol_u(sig_u);
        const FlatVol vol_d(sig_d);

        struct WelfordStat
        {
            Real mean = 0.0;
            Real M2 = 0.0;
        };

        WelfordStat w_base, w_Su, w_Sd, w_vu, w_vd, w_Tu, w_Td, w_ru, w_rd;
        int cnt = 0;

        auto wf_push = [](WelfordStat &w, Real x, int n)
        {
            const Real delta = x - w.mean;
            w.mean += delta / static_cast<Real>(n);
            w.M2 += delta * (x - w.mean);
        };

        // ── Monte Carlo loop ──────────────────────────────────────────────────
        const int N = settings.mc_paths;
        for (int i = 0; i < N; ++i)
        {
            // Draw one path's worth of Gaussians and BB-uniforms
            for (int j = 0; j < n_steps; ++j)
            {
                zs[j] = gauss(rng_gauss);
                us[j] = uniform01(rng_bb);
            }

            ++cnt;
            // Centre (base) + 8 bumped variants (CRN: all share zs and us)
            wf_push(w_base, sim_one(S0, vol_base, r, T), cnt);
            wf_push(w_Su, sim_one(S0 + dS, vol_base, r, T), cnt);
            wf_push(w_Sd, sim_one(S0 - dS, vol_base, r, T), cnt);
            wf_push(w_vu, sim_one(S0, vol_u, r, T), cnt);
            wf_push(w_vd, sim_one(S0, vol_d, r, T), cnt);
            wf_push(w_Tu, sim_one(S0, vol_base, r, T_up), cnt);
            wf_push(w_Td, sim_one(S0, vol_base, r, T_dn), cnt);
            wf_push(w_ru, sim_one(S0, vol_base, r_up, T), cnt);
            wf_push(w_rd, sim_one(S0, vol_base, r_dn, T), cnt);
        }

        // ── standard error helper ─────────────────────────────────────────────
        auto se = [&](const WelfordStat &w) -> Real
        {
            if (cnt < 2)
                return 0.0;
            const Real var = w.M2 / static_cast<Real>(cnt - 1);
            return std::sqrt(var / static_cast<Real>(cnt));
        };

        // Propagated SE for a central-FD estimator (V+ − V−) / (2·bump)
        auto fd_se = [&](const WelfordStat &wu, const WelfordStat &wd, Real bump) -> Real
        {
            const Real su = se(wu), sd = se(wd);
            return std::sqrt(su * su + sd * sd) / (2.0 * bump);
        };

        // ── assemble result ───────────────────────────────────────────────────
        PricingResult out;
        out.npv = w_base.mean;
        out.mc_std_error = se(w_base);

        // Central FD Greeks
        // delta / gamma — bump spot
        out.greeks.delta = (w_Su.mean - w_Sd.mean) / (2.0 * dS);
        out.greeks.gamma = (w_Su.mean - 2.0 * w_base.mean + w_Sd.mean) / (dS * dS);
        // vega  — bump vol, report as dP/dσ (per unit vol)
        out.greeks.vega = (w_vu.mean - w_vd.mean) / (2.0 * dv);
        // theta — bump time, report as -dP/dT (value decay per unit time)
        out.greeks.theta = -(w_Tu.mean - w_Td.mean) / (2.0 * dT);
        // rho   — bump rate, report as dP/dr (per unit rate)
        out.greeks.rho = (w_ru.mean - w_rd.mean) / (2.0 * dr);

        // Standard errors on Greeks (propagated from MC noise)
        out.greeks.delta_std_error = fd_se(w_Su, w_Sd, dS);
        out.greeks.gamma_std_error = fd_se(w_Su, w_Sd, dS);
        out.greeks.vega_std_error = fd_se(w_vu, w_vd, dv);
        out.greeks.theta_std_error = fd_se(w_Tu, w_Td, dT);
        out.greeks.rho_std_error = fd_se(w_ru, w_rd, dr);

        // Diagnostics string
        const auto bt_str = [&]() -> std::string
        {
            switch (opt.barrier_type)
            {
            case BarrierType::UpAndIn:
                return "up-and-in";
            case BarrierType::UpAndOut:
                return "up-and-out";
            case BarrierType::DownAndIn:
                return "down-and-in";
            case BarrierType::DownAndOut:
                return "down-and-out";
            }
            return "?";
        };

        out.diagnostics = "Barrier MC (BS): " + bt_str() + ", H=" + std::to_string(H) + ", K=" + std::to_string(K) + ", T=" + std::to_string(T) + ", paths=" + std::to_string(N) + ", steps/path=" + std::to_string(n_steps) + (opt.brownian_bridge ? " [BB corrected]" : " [discrete]");

        res_ = out;
    }

} // namespace quantModeling
