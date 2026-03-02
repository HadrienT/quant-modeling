#include "quantModeling/engines/mc/autocall.hpp"

#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────
    //  Autocall MC pricing
    // ─────────────────────────────────────────────────────────────────────

    void BSAutocallMCEngine::visit(const AutocallNote &note)
    {
        const auto &m = require_model<ILocalVolModel>("BSAutocallMCEngine");
        const PricingSettings &settings = ctx_.settings;

        // ── Validate ──────────────────────────────────────────────────
        if (note.observation_dates.empty())
            throw InvalidInput("AutocallNote: need at least 1 observation date");
        if (note.notional <= 0.0)
            throw InvalidInput("AutocallNote: notional must be > 0");
        if (note.autocall_barrier <= 0.0)
            throw InvalidInput("AutocallNote: autocall_barrier must be > 0");
        if (note.coupon_rate < 0.0)
            throw InvalidInput("AutocallNote: coupon_rate must be ≥ 0");

        const auto n_obs = note.observation_dates.size();
        const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 42);

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma(); // flat vol

        // ── Precompute drift and diffusion per observation date ───────
        // S(T_i) / S(T_{i-1}) = exp((r-q-0.5σ²)(T_i-T_{i-1}) + σ√(T_i-T_{i-1}) Z_i)
        struct StepSpec
        {
            Real drift;    // (r-q-0.5σ²) × dt
            Real vol_sqrt; // σ × √dt
            Real df;       // e^{-r T_i} — discount to time 0
            Real time;     // T_i
        };
        std::vector<StepSpec> steps(n_obs);
        {
            Real t_prev = 0.0;
            const Real base_drift = r - q - 0.5 * sigma * sigma;
            for (std::size_t i = 0; i < n_obs; ++i)
            {
                const Real dt = note.observation_dates[i] - t_prev;
                if (dt <= 0.0)
                    throw InvalidInput("AutocallNote: observation_dates must be strictly increasing and > 0");
                steps[i].drift = base_drift * dt;
                steps[i].vol_sqrt = sigma * std::sqrt(dt);
                steps[i].df = m.discount_curve().discount(note.observation_dates[i]);
                steps[i].time = note.observation_dates[i];
                t_prev = note.observation_dates[i];
            }
        }

        // ── Barrier levels in absolute terms ─────────────────────────
        const Real ac_level = note.autocall_barrier * S0;
        const Real cpn_level = note.coupon_barrier * S0;
        const Real put_level = note.put_barrier * S0;

        // ── Monte Carlo loop ─────────────────────────────────────────
        RngFactory rng_fact(seed);

        Real sum = 0.0;
        Real sum2 = 0.0;

        for (int p = 0; p < n_paths; ++p)
        {
            Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
            NormalBoxMuller normal;

            Real S = S0;
            bool knocked_in = false; // put barrier breached
            int missed_coupons = 0;  // for memory coupon
            bool called = false;
            Real path_pv = 0.0;

            for (std::size_t i = 0; i < n_obs; ++i)
            {
                const Real Z = normal(rng);
                S = S * std::exp(steps[i].drift + steps[i].vol_sqrt * Z);

                // ── Knock-in put check ────────────────────────────────
                if (note.ki_continuous && S < put_level)
                    knocked_in = true;

                // ── Autocall check ────────────────────────────────────
                if (S >= ac_level)
                {
                    // Coupon: if memory, pay all missed + current; else just current
                    int cpn_periods = note.memory_coupon ? (missed_coupons + 1) : 1;
                    Real cashflow = note.notional *
                                    (1.0 + note.coupon_rate * static_cast<Real>(cpn_periods));
                    path_pv = cashflow * steps[i].df;
                    called = true;
                    break;
                }

                // ── Coupon barrier check (no autocall) ────────────────
                if (S >= cpn_level)
                {
                    int cpn_periods = note.memory_coupon ? (missed_coupons + 1) : 1;
                    path_pv += note.notional * note.coupon_rate *
                               static_cast<Real>(cpn_periods) * steps[i].df;
                    missed_coupons = 0;
                }
                else
                {
                    ++missed_coupons;
                }
            }

            // ── If not called, settle at final date ──────────────────
            if (!called)
            {
                const Real df_final = steps[n_obs - 1].df;

                // Final knock-in check (if not continuous)
                if (!note.ki_continuous && S < put_level)
                    knocked_in = true;

                if (knocked_in)
                {
                    // Investor suffers loss: receives notional × S/S0
                    path_pv += note.notional * (S / S0) * df_final;
                }
                else
                {
                    // Capital protected — receive notional
                    path_pv += note.notional * df_final;
                }
            }

            sum += path_pv;
            sum2 += path_pv * path_pv;
        }

        const auto N = static_cast<Real>(n_paths);
        const Real mean = sum / N;
        const Real var = sum2 / N - mean * mean;

        PricingResult out;
        out.npv = mean;
        out.mc_std_error = std::sqrt(var / N);
        out.diagnostics = "BSAutocallMCEngine (" + m.model_name() +
                          ", paths=" + std::to_string(n_paths) +
                          ", obs=" + std::to_string(n_obs) + ")";
        res_ = out;
    }

} // namespace quantModeling
