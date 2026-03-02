#include "quantModeling/engines/mc/lookback.hpp"

#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    void BSEuroLookbackMCEngine::validate(const LookbackOption &opt, int n_paths)
    {
        if (!opt.payoff)
            throw InvalidInput("LookbackOption: payoff is null");
        if (!opt.exercise || opt.exercise->dates().empty())
            throw InvalidInput("LookbackOption: exercise is null or has no dates");
        if (opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument("BSEuroLookbackMCEngine: only European exercise is supported");
        if (opt.exercise->dates().front() <= 0.0)
            throw InvalidInput("LookbackOption: maturity must be > 0");
        if (opt.notional == 0.0)
            throw InvalidInput("LookbackOption: notional must be non-zero");
        if (n_paths <= 0)
            throw InvalidInput("LookbackOption: mc_paths must be > 0");
    }

    void BSEuroLookbackMCEngine::visit(const LookbackOption &opt)
    {
        const auto &m = require_model<ILocalVolModel>("BSEuroLookbackMCEngine");
        PricingSettings settings = ctx_.settings;
        validate(opt, settings.mc_paths);

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();
        const Real T = opt.exercise->dates().front();
        const Real K = opt.payoff->strike();
        const OptionType optType = opt.payoff->type();

        const bool is_float = (opt.style == LookbackStyle::FloatingStrike);
        // For floating-strike the relevant extremum is determined by option type:
        //   Float Call -> S_T - S_min  (always >= 0)
        //   Float Put  -> S_max - S_T  (always >= 0)
        // For fixed-strike the user-supplied opt.extremum is respected.

        // --- Time-step grid ---
        const int n_steps = (opt.n_steps > 0)
                                ? opt.n_steps
                                : std::max(1, static_cast<int>(252.0 * T + 0.5));
        const Real dt = T / static_cast<Real>(n_steps);
        const Real df = m.discount_curve().discount(T);

        // --- FD bump grids for gamma / theta (common random numbers) ---
        const GreeksBumps bumps;
        const Real dS = S0 * bumps.delta_bump;
        const Real S_up = S0 + dS;
        const Real S_dn = S0 - dS;
        const Real T_up = T + bumps.theta_bump;
        const Real T_dn = std::max(1e-8, T - bumps.theta_bump);

        const int n_steps_Tup = (opt.n_steps > 0) ? opt.n_steps
                                                  : std::max(1, static_cast<int>(252.0 * T_up + 0.5));
        const int n_steps_Tdn = (opt.n_steps > 0) ? opt.n_steps
                                                  : std::max(1, static_cast<int>(252.0 * T_dn + 0.5));

        const Real dt_Tup = T_up / static_cast<Real>(n_steps_Tup);
        const Real dt_Tdn = T_dn / static_cast<Real>(n_steps_Tdn);
        const Real df_Tup = m.discount_curve().discount(T_up);
        const Real df_Tdn = m.discount_curve().discount(T_dn);

        // Per-step volatility lookup — works for flat vol and local-vol surfaces.
        // For FlatVol, vol.value(S, t) == sigma_  for all S, t (numerically equivalent
        // to the old precomputed-constant approach).
        const IVolatility &vol = m.vol();
        const Real sqrt_dt = std::sqrt(dt);
        const Real sqrt_dt_up = std::sqrt(dt_Tup);
        const Real sqrt_dt_dn = std::sqrt(dt_Tdn);
        // sigma (already declared above) is used for the LRM score-function
        // approximations (vega/rho) which assume flat vol.

        // --- Payoff helper ---
        auto compute_payoff = [&](Real ST, Real path_min, Real path_max) -> Real
        {
            if (is_float)
            {
                return (optType == OptionType::Call) ? (ST - path_min) : (path_max - ST);
            }
            const Real extreme = (opt.extremum == LookbackExtremum::Minimum) ? path_min : path_max;
            return (optType == OptionType::Call) ? std::max(extreme - K, 0.0)
                                                 : std::max(K - extreme, 0.0);
        };

        // --- Pathwise delta: dPayoff/dS0 ---
        // All spot paths scale linearly with S0, so ST -> ST*c, path_min -> path_min*c, path_max -> path_max*c.
        // For float: d(pv)/dS0 = pv/S0.
        // For fixed ITM: d(extreme - K)/dS0 = extreme/S0.
        auto compute_pathwise_delta = [&](Real ST, Real path_min, Real path_max, Real pv) -> Real
        {
            if (is_float)
                return df * pv / S0;

            if (pv == 0.0)
                return 0.0; // OTM: derivative is 0 a.e.

            const Real extreme = (opt.extremum == LookbackExtremum::Minimum) ? path_min : path_max;
            return df * extreme / S0;
        };

        // --- Welford state ---
        Real meanPayoff = 0.0, M2_payoff = 0.0;
        Real meanDelta = 0.0, M2_delta = 0.0;
        Real meanVega = 0.0, M2_vega = 0.0;
        Real meanRho = 0.0, M2_rho = 0.0;
        Real meanGamma = 0.0, M2_gamma = 0.0;
        Real meanTheta = 0.0, M2_theta = 0.0;
        int n = 0;

        auto welford_update = [&](Real x, Real &mean, Real &M2, int n_val)
        {
            const Real delta = x - mean;
            mean += delta / static_cast<Real>(n_val);
            M2 += delta * (x - mean);
        };

        // --- RNG ---
        RngFactory rng_factory(static_cast<uint64_t>(settings.mc_seed));
        Pcg32 rng = rng_factory.make(0);
        AntitheticGaussianGenerator gauss;
        if (settings.mc_antithetic)
            gauss.enable_antithetic();

        // --- Monte Carlo loop ---
        const int max_steps = std::max(n_steps, std::max(n_steps_Tup, n_steps_Tdn));

        for (int i = 0; i < settings.mc_paths; ++i)
        {
            // Simulate base path + T-bumped paths in one pass (common random numbers for theta).
            Real S_base = S0, path_min = S0, path_max = S0;
            Real S_Tup = S0, min_Tup = S0, max_Tup = S0;
            Real S_Tdn = S0, min_Tdn = S0, max_Tdn = S0;

            for (int j = 0; j < max_steps; ++j)
            {
                const Real z = gauss(rng);
                if (j < n_steps)
                {
                    const Real t_cur = static_cast<Real>(j) * dt;
                    const Real sig = vol.value(S_base, t_cur);
                    S_base *= std::exp((r - q - 0.5 * sig * sig) * dt + sig * sqrt_dt * z);
                    path_min = std::min(path_min, S_base);
                    path_max = std::max(path_max, S_base);
                }
                if (j < n_steps_Tup)
                {
                    const Real t_cur = static_cast<Real>(j) * dt_Tup;
                    const Real sig = vol.value(S_Tup, t_cur);
                    S_Tup *= std::exp((r - q - 0.5 * sig * sig) * dt_Tup + sig * sqrt_dt_up * z);
                    min_Tup = std::min(min_Tup, S_Tup);
                    max_Tup = std::max(max_Tup, S_Tup);
                }
                if (j < n_steps_Tdn)
                {
                    const Real t_cur = static_cast<Real>(j) * dt_Tdn;
                    const Real sig = vol.value(S_Tdn, t_cur);
                    S_Tdn *= std::exp((r - q - 0.5 * sig * sig) * dt_Tdn + sig * sqrt_dt_dn * z);
                    min_Tdn = std::min(min_Tdn, S_Tdn);
                    max_Tdn = std::max(max_Tdn, S_Tdn);
                }
            }

            const Real pv = compute_payoff(S_base, path_min, path_max);
            const Real delta_pv = compute_pathwise_delta(S_base, path_min, path_max, pv);

            // FD gamma (CRN): scale base path by S_up/S0 and S_dn/S0.
            // Spot-scaling argument: multiplying S0 by c scales all path values by c.
            const Real sc_up = S_up / S0;
            const Real sc_dn = S_dn / S0;
            const Real pv_Sup = compute_payoff(S_base * sc_up, path_min * sc_up, path_max * sc_up);
            const Real pv_Sdn = compute_payoff(S_base * sc_dn, path_min * sc_dn, path_max * sc_dn);
            const Real gamma_path = df * (pv_Sup - 2.0 * pv + pv_Sdn) / (dS * dS);

            // FD theta (CRN): use the T-bumped paths simulated above.
            const Real pv_Tup = compute_payoff(S_Tup, min_Tup, max_Tup);
            const Real pv_Tdn = compute_payoff(S_Tdn, min_Tdn, max_Tdn);
            const Real theta_path = (df_Tdn * pv_Tdn - df_Tup * pv_Tup) / (2.0 * bumps.theta_bump);

            // LRM scores — approximate using terminal log-return (consistent with Asian engine).
            const Real log_ret = std::log(S_base / S0);
            Real score_sigma = 0.0, score_r = 0.0;
            if (sigma > 1e-10)
            {
                score_sigma = (log_ret * log_ret) / (sigma * T) - 0.5 * T / sigma;
                score_r = (log_ret * T) / (sigma * sigma);
            }
            const Real vega_path = pv * score_sigma;
            const Real rho_path = -T * pv + pv * score_r;

            ++n;
            welford_update(pv, meanPayoff, M2_payoff, n);
            welford_update(delta_pv, meanDelta, M2_delta, n);
            welford_update(vega_path, meanVega, M2_vega, n);
            welford_update(rho_path, meanRho, M2_rho, n);
            welford_update(gamma_path, meanGamma, M2_gamma, n);
            welford_update(theta_path, meanTheta, M2_theta, n);
        }

        // --- Assemble result ---
        auto std_err = [&](Real M2_val) -> Real
        {
            return (n > 1) ? std::sqrt((M2_val / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
        };

        const Real disc = m.discount_curve().discount(T);
        PricingResult out;
        out.npv = opt.notional * disc * meanPayoff;
        out.mc_std_error = opt.notional * disc * std_err(M2_payoff);

        out.greeks.delta = opt.notional * meanDelta;
        out.greeks.delta_std_error = opt.notional * std_err(M2_delta);

        out.greeks.vega = opt.notional * disc * meanVega;
        out.greeks.vega_std_error = opt.notional * disc * std_err(M2_vega);

        out.greeks.rho = opt.notional * disc * meanRho;
        out.greeks.rho_std_error = opt.notional * disc * std_err(M2_rho);

        out.greeks.gamma = opt.notional * meanGamma;
        out.greeks.gamma_std_error = opt.notional * std_err(M2_gamma);

        out.greeks.theta = opt.notional * meanTheta;
        out.greeks.theta_std_error = opt.notional * std_err(M2_theta);

        out.diagnostics =
            std::string("BS MC European Lookback (flat r,q,sigma)") +
            (settings.mc_antithetic ? " + antithetic" : "") +
            ": style=" + (opt.style == LookbackStyle::FixedStrike ? "fixed" : "floating") +
            ", extremum=" + (opt.extremum == LookbackExtremum::Minimum ? "min" : "max") +
            ", K=" + std::to_string(K) +
            ", T=" + std::to_string(T) +
            ", paths=" + std::to_string(settings.mc_paths) +
            ", steps/path=" + std::to_string(n_steps);

        res_ = out;
    }

} // namespace quantModeling
