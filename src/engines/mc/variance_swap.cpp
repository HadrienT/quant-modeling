#include "quantModeling/engines/mc/variance_swap.hpp"

#include "quantModeling/instruments/equity/variance_swap.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{

    // ─── helpers ─────────────────────────────────────────────────────────────

    namespace
    {
        /// Build the monitoring schedule.  If the instrument has an explicit
        /// observation_dates vector, use it; otherwise default to daily (252/yr).
        std::vector<Time> make_schedule(Time T, const std::vector<Time> &obs)
        {
            if (!obs.empty())
                return obs;
            const int n = std::max(1, static_cast<int>(252.0 * T));
            std::vector<Time> dates(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                dates[static_cast<std::size_t>(i)] = T * static_cast<Real>(i + 1) / static_cast<Real>(n);
            return dates;
        }

        /// Simulate one GBM path and return annualised realised variance.
        /// Uses discrete log-returns: σ² = (1/T) Σ (ln S_i/S_{i-1})²  (with
        /// de-meaning if desired — here we use the standard definition without
        /// mean correction, consistent with variance swap practice).
        struct RealisedVolResult
        {
            Real variance;
            Real volatility;
        };

        RealisedVolResult simulate_realised(const ILocalVolModel &m,
                                            const std::vector<Time> &sched,
                                            Pcg32 &rng, NormalBoxMuller &normal)
        {
            const Real S0 = m.spot0();
            const Real r = m.rate_r();
            const Real q = m.yield_q();
            const Real sigma = m.vol_sigma();
            const Real drift_base = r - q - 0.5 * sigma * sigma;

            Real S = S0;
            Real sum_log2 = 0.0;
            Time t_prev = 0.0;
            const auto n = sched.size();

            for (std::size_t i = 0; i < n; ++i)
            {
                const Real dt = sched[i] - t_prev;
                const Real Z = normal(rng);
                const Real log_ret = drift_base * dt + sigma * std::sqrt(dt) * Z;
                S = S * std::exp(log_ret);
                sum_log2 += log_ret * log_ret;
                t_prev = sched[i];
            }

            const Real T = sched.back();
            const Real var = sum_log2 / T;
            return {var, std::sqrt(var)};
        }
    } // anonymous namespace

    // ─── Variance swap visit ─────────────────────────────────────────────────

    void VolSwapMCEngine::visit(const VarianceSwap &vs)
    {
        const auto &m = require_model<ILocalVolModel>("VolSwapMCEngine");
        const PricingSettings &settings = ctx_.settings;

        if (vs.maturity <= 0.0)
            throw InvalidInput("VarianceSwap: maturity must be > 0");

        const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 42);

        const auto sched = make_schedule(vs.maturity, vs.observation_dates);
        const Real T = vs.maturity;
        const Real r = m.rate_r();
        const Real df = m.discount_curve().discount(T);

        RngFactory rng_fact(seed);
        Real sum = 0.0, sum2 = 0.0;

        for (int p = 0; p < n_paths; ++p)
        {
            Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
            NormalBoxMuller normal;
            auto [var, vol] = simulate_realised(m, sched, rng, normal);
            const Real pv = vs.notional * (var - vs.strike_var) * df;
            sum += pv;
            sum2 += pv * pv;
        }

        const auto N = static_cast<Real>(n_paths);
        const Real mean = sum / N;
        const Real variance = sum2 / N - mean * mean;

        PricingResult out;
        out.npv = mean;
        out.mc_std_error = std::sqrt(variance / N);
        out.diagnostics = "VolSwapMCEngine:VarianceSwap (paths=" +
                          std::to_string(n_paths) + ", obs=" +
                          std::to_string(sched.size()) + ")";
        res_ = out;
    }

    // ─── Volatility swap visit ───────────────────────────────────────────────

    void VolSwapMCEngine::visit(const VolatilitySwap &vs)
    {
        const auto &m = require_model<ILocalVolModel>("VolSwapMCEngine");
        const PricingSettings &settings = ctx_.settings;

        if (vs.maturity <= 0.0)
            throw InvalidInput("VolatilitySwap: maturity must be > 0");

        const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 42);

        const auto sched = make_schedule(vs.maturity, vs.observation_dates);
        const Real T = vs.maturity;
        const Real r = m.rate_r();
        const Real df = m.discount_curve().discount(T);

        RngFactory rng_fact(seed);
        Real sum = 0.0, sum2 = 0.0;

        for (int p = 0; p < n_paths; ++p)
        {
            Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
            NormalBoxMuller normal;
            auto [var, vol] = simulate_realised(m, sched, rng, normal);
            const Real pv = vs.notional * (vol - vs.strike_vol) * df;
            sum += pv;
            sum2 += pv * pv;
        }

        const auto N = static_cast<Real>(n_paths);
        const Real mean = sum / N;
        const Real variance = sum2 / N - mean * mean;

        PricingResult out;
        out.npv = mean;
        out.mc_std_error = std::sqrt(variance / N);
        out.diagnostics = "VolSwapMCEngine:VolatilitySwap (paths=" +
                          std::to_string(n_paths) + ", obs=" +
                          std::to_string(sched.size()) + ")";
        res_ = out;
    }

    // ─── rejections ──────────────────────────────────────────────────────────

    void VolSwapMCEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void VolSwapMCEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void VolSwapMCEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void VolSwapMCEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void VolSwapMCEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void VolSwapMCEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void VolSwapMCEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
