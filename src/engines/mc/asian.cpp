#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"
#include <cmath>
#include <stdexcept>
#include <vector>

namespace quantModeling
{

    void BSEuroAsianMCEngine::visit(const AsianOption &opt)
    {
        validate(opt);
        const auto &m = require_model<IBlackScholesModel>("BSEuroAsianMCEngine");
        PricingSettings settings = ctx_.settings;

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();

        const Real T = opt.exercise->dates().front();
        const auto &payoff = *opt.payoff;
        const OptionType optType = payoff.type();
        const Real K = payoff.strike();

        const int num_dates = std::max(1, static_cast<int>(T * 252.0 + 0.5));

        RngFactory rngFact(settings.mc_seed);
        Pcg32 rng = rngFact.make(0);
        AntitheticGaussianGenerator gaussianGenerator;

        // Welford's online algorithm for computing mean and variance of payoff and pathwise delta
        Real meanPayoff = 0.0;
        Real meanDelta = 0.0;
        Real M2_payoff = 0.0;
        Real M2_delta = 0.0;
        // Welford for LRM estimators
        Real meanVega = 0.0;
        Real M2_vega = 0.0;
        Real meanRho = 0.0;
        Real M2_rho = 0.0;
        // Welford for gamma estimator (FD with common random numbers)
        Real meanGamma = 0.0;
        Real M2_gamma = 0.0;
        // Welford for theta estimator (FD with common random numbers)
        Real meanTheta = 0.0;
        Real M2_theta = 0.0;
        int n = 0;

        auto welford_update = [&](Real x, Real &mean, Real &M2, int n_val)
        {
            const Real delta = x - mean;
            mean += delta / static_cast<Real>(n_val);
            const Real delta2 = x - mean;
            M2 += delta * delta2;
        };

        auto welford_push = [&](Real payoff_val, Real delta_val)
        {
            ++n;
            welford_update(payoff_val, meanPayoff, M2_payoff, n);
            welford_update(delta_val, meanDelta, M2_delta, n);
        };

        const bool is_arithmetic = (opt.average_type == AsianAverageType::Arithmetic);

        // Configure generator: enable antithetic if requested
        if (settings.mc_antithetic)
        {
            gaussianGenerator.enable_antithetic();
        }
        else
        {
            gaussianGenerator.disable_antithetic();
        }

        const Real df = std::exp(-r * T);

        // FD bumps for gamma/theta (common random numbers)
        const GreeksBumps bumps;
        const Real dS = S0 * bumps.delta_bump;
        const Real S0_up = S0 + dS;
        const Real S0_dn = S0 - dS;
        const Real T_up = T + bumps.theta_bump;
        const Real T_dn = std::max(1e-8, T - bumps.theta_bump);

        const int num_dates_up = std::max(1, static_cast<int>(T_up * 252.0 + 0.5));
        const int num_dates_dn = std::max(1, static_cast<int>(T_dn * 252.0 + 0.5));

        const Real dt = T / static_cast<Real>(num_dates);
        const Real dt_up = T_up / static_cast<Real>(num_dates_up);
        const Real dt_dn = T_dn / static_cast<Real>(num_dates_dn);

        const Real sigma_sqrt_dt = sigma * std::sqrt(dt);
        const Real sigma_sqrt_dt_up = sigma * std::sqrt(dt_up);
        const Real sigma_sqrt_dt_dn = sigma * std::sqrt(dt_dn);

        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real drift_up = (r - q - 0.5 * sigma * sigma) * dt_up;
        const Real drift_dn = (r - q - 0.5 * sigma * sigma) * dt_dn;

        const Real exp_drift = std::exp(drift);
        const Real exp_drift_up = std::exp(drift_up);
        const Real exp_drift_dn = std::exp(drift_dn);

        const Real df_up = std::exp(-r * T_up);
        const Real df_dn = std::exp(-r * T_dn);

        // Means for FD gamma/theta
        Real meanPayoff_Sup = 0.0;
        Real meanPayoff_Sdn = 0.0;
        Real meanPayoff_Tup = 0.0;
        Real meanPayoff_Tdn = 0.0;
        Real M2_payoff_Sup = 0.0;
        Real M2_payoff_Sdn = 0.0;
        Real M2_payoff_Tup = 0.0;
        Real M2_payoff_Tdn = 0.0;

        // Run paths and accumulate payoff + pathwise delta
        for (int i = 0; i < settings.mc_paths; ++i)
        {
            // Simulate base path and T up/down in one pass using common random numbers
            Real S = S0;
            Real S_up = S0;
            Real S_dn = S0;

            Real sum_S = 0.0;
            Real sum_S_up = 0.0;
            Real sum_S_dn = 0.0;
            Real sum_log_S = 0.0;
            Real sum_log_S_up = 0.0;
            Real sum_log_S_dn = 0.0;

            const int max_dates = std::max(num_dates, std::max(num_dates_up, num_dates_dn));
            for (int j = 0; j < max_dates; ++j)
            {
                const Real z = gaussianGenerator(rng);
                if (j < num_dates)
                {
                    S = S * exp_drift * std::exp(sigma_sqrt_dt * z);
                    if (is_arithmetic)
                        sum_S += S;
                    else
                        sum_log_S += std::log(S);
                }
                if (j < num_dates_up)
                {
                    S_up = S_up * exp_drift_up * std::exp(sigma_sqrt_dt_up * z);
                    if (is_arithmetic)
                        sum_S_up += S_up;
                    else
                        sum_log_S_up += std::log(S_up);
                }
                if (j < num_dates_dn)
                {
                    S_dn = S_dn * exp_drift_dn * std::exp(sigma_sqrt_dt_dn * z);
                    if (is_arithmetic)
                        sum_S_dn += S_dn;
                    else
                        sum_log_S_dn += std::log(S_dn);
                }
            }

            Real average = 0.0;
            Real average_Tup = 0.0;
            Real average_Tdn = 0.0;
            if (is_arithmetic)
            {
                average = sum_S / static_cast<Real>(num_dates);
                average_Tup = sum_S_up / static_cast<Real>(num_dates_up);
                average_Tdn = sum_S_dn / static_cast<Real>(num_dates_dn);
            }
            else
            {
                average = std::exp(sum_log_S / static_cast<Real>(num_dates));
                average_Tup = std::exp(sum_log_S_up / static_cast<Real>(num_dates_up));
                average_Tdn = std::exp(sum_log_S_dn / static_cast<Real>(num_dates_dn));
            }

            const Real payoff_val = payoff(average);

            // Pathwise delta: d(Payoff(Average))/d(Average) × d(Average)/dS0 × discount
            // For arithmetic: d(Average)/dS0 = (1/num_dates) × sum(S_t/S0)
            // We approximate: d(Average)/dS0 ≈ Average / S0 (simplification, suffices for ATM)
            Real delta_val = 0.0;
            if (optType == OptionType::Call && average > K)
            {
                delta_val = df * (average / S0);
            }
            else if (optType == OptionType::Put && average < K)
            {
                delta_val = -df * (average / S0);
            }

            welford_push(payoff_val, delta_val);

            // FD gamma payoffs using S0 scaling
            const Real factor_up = S0_up / S0;
            const Real factor_dn = S0_dn / S0;
            const Real payoff_up = payoff(average * factor_up);
            const Real payoff_dn = payoff(average * factor_dn);

            // FD theta payoffs using T up/down
            const Real payoff_Tup = payoff(average_Tup);
            const Real payoff_Tdn = payoff(average_Tdn);

            const Real gamma_path = df * (payoff_up - 2.0 * payoff_val + payoff_dn) / (dS * dS);
            const Real theta_path = (df_dn * payoff_Tdn - df_up * payoff_Tup) / (2.0 * bumps.theta_bump);

            welford_update(payoff_up, meanPayoff_Sup, M2_payoff_Sup, n);
            welford_update(payoff_dn, meanPayoff_Sdn, M2_payoff_Sdn, n);
            welford_update(payoff_Tup, meanPayoff_Tup, M2_payoff_Tup, n);
            welford_update(payoff_Tdn, meanPayoff_Tdn, M2_payoff_Tdn, n);

            // LRM score functions for Asian: approximate using average
            // For a path with average A, approximate d ln p / d sigma and d ln p / d r
            // using the sensitivity of A to these parameters
            // Simplified: use (A - S0) / S0 as proxy for log-return to compute scores
            const Real log_avg = std::log(average / S0);
            Real score_sigma = 0.0;
            Real score_r = 0.0;

            if (sigma > 1e-10)
            {
                // Approximate: d ln p / d sigma ~ (log_avg)^2 / (sigma * T) - 0.5*T
                // and d ln p / d r ~ log_avg * T / sigma^2
                score_sigma = (log_avg * log_avg) / (sigma * T) - 0.5 * T / sigma;
                score_r = (log_avg * T) / (sigma * sigma);
            }

            const Real path_vega = payoff_val * score_sigma;
            const Real path_rho = -T * payoff_val + payoff_val * score_r;

            // Update vega/rho with the same helper
            welford_update(path_vega, meanVega, M2_vega, n);
            welford_update(path_rho, meanRho, M2_rho, n);
            welford_update(gamma_path, meanGamma, M2_gamma, n);
            welford_update(theta_path, meanTheta, M2_theta, n);
        }

        Real sampleVariance = 0.0;
        Real varMean = 0.0;
        if (n > 1)
        {
            sampleVariance = M2_payoff / static_cast<Real>(n - 1);
            varMean = sampleVariance / static_cast<Real>(n);
        }

        const Real disc = std::exp(-r * T);
        const Real price = disc * meanPayoff;
        const Real priceStdError = (n > 1) ? disc * std::sqrt(varMean) : 0.0;

        PricingResult out;
        out.diagnostics = settings.mc_antithetic
                              ? "BS MC European Asian (flat r,q,sigma) + antithetic"
                              : "BS MC European Asian (flat r,q,sigma)";
        out.npv = opt.notional * price;
        out.mc_std_error = opt.notional * priceStdError;

        out.greeks.delta = opt.notional * meanDelta;
        // Delta standard error from Welford stats
        Real deltaSampleVar = 0.0;
        Real deltaVarMean = 0.0;
        if (n > 1)
        {
            deltaSampleVar = M2_delta / static_cast<Real>(n - 1);
            deltaVarMean = deltaSampleVar / static_cast<Real>(n);
        }
        const Real deltaStdError = (n > 1) ? std::sqrt(deltaVarMean) * opt.notional : 0.0;
        out.greeks.delta_std_error = deltaStdError;

        // Vega: LRM estimator with std error
        const Real vegaStdErr = (n > 1) ? std::sqrt((M2_vega / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
        out.greeks.vega = opt.notional * disc * meanVega;
        out.greeks.vega_std_error = opt.notional * disc * vegaStdErr;

        // Rho: LRM estimator with std error
        const Real rhoStdErr = (n > 1) ? std::sqrt((M2_rho / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
        out.greeks.rho = opt.notional * disc * meanRho;
        out.greeks.rho_std_error = opt.notional * disc * rhoStdErr;

        const Real gammaStdErr = (n > 1) ? std::sqrt((M2_gamma / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
        out.greeks.gamma = opt.notional * meanGamma;
        out.greeks.gamma_std_error = opt.notional * gammaStdErr;

        const Real thetaStdErr = (n > 1) ? std::sqrt((M2_theta / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
        out.greeks.theta = opt.notional * meanTheta;
        out.greeks.theta_std_error = opt.notional * thetaStdErr;

        res_ = out;
    }

    Real BSEuroAsianMCEngine::simulate_arithmetic_path(
        Real S0, Real T, Real r, Real q, Real sigma,
        int num_dates,
        const IPayoff &payoff,
        Pcg32 &rng,
        AntitheticGaussianGenerator &gaussian_gen)
    {
        const Real dt = T / static_cast<Real>(num_dates);
        const Real sigma_sqrt_dt = sigma * std::sqrt(dt);
        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real exp_drift = std::exp(drift);

        Real S = S0;
        Real sum_S = 0.0;

        for (int j = 0; j < num_dates; ++j)
        {
            const Real z = gaussian_gen(rng);
            S = S * exp_drift * std::exp(sigma_sqrt_dt * z);
            sum_S += S;
        }

        const Real average_S = sum_S / static_cast<Real>(num_dates);
        return payoff(average_S);
    }

    Real BSEuroAsianMCEngine::simulate_geometric_path(
        Real S0, Real T, Real r, Real q, Real sigma,
        int num_dates,
        const IPayoff &payoff,
        Pcg32 &rng,
        AntitheticGaussianGenerator &gaussian_gen)
    {
        const Real dt = T / static_cast<Real>(num_dates);
        const Real sigma_sqrt_dt = sigma * std::sqrt(dt);
        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real exp_drift = std::exp(drift);

        Real S = S0;
        Real sum_log_S = 0.0;

        for (int j = 0; j < num_dates; ++j)
        {
            const Real z = gaussian_gen(rng);
            S = S * exp_drift * std::exp(sigma_sqrt_dt * z);
            sum_log_S += std::log(S);
        }

        // Geometric mean = exp(average of log prices)
        const Real log_geometric_mean = sum_log_S / static_cast<Real>(num_dates);
        const Real geometric_mean = std::exp(log_geometric_mean);

        return payoff(geometric_mean);
    }

    std::pair<Real, Real> BSEuroAsianMCEngine::simulate_arithmetic_path_with_average(
        Real S0, Real T, Real r, Real q, Real sigma,
        int num_dates,
        const IPayoff &payoff,
        Pcg32 &rng,
        AntitheticGaussianGenerator &gaussian_gen)
    {
        const Real dt = T / static_cast<Real>(num_dates);
        const Real sigma_sqrt_dt = sigma * std::sqrt(dt);
        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real exp_drift = std::exp(drift);

        Real S = S0;
        Real sum_S = 0.0;

        for (int j = 0; j < num_dates; ++j)
        {
            const Real z = gaussian_gen(rng);
            S = S * exp_drift * std::exp(sigma_sqrt_dt * z);
            sum_S += S;
        }

        const Real average_S = sum_S / static_cast<Real>(num_dates);
        const Real payoff_val = payoff(average_S);

        return std::make_pair(payoff_val, average_S);
    }

    std::pair<Real, Real> BSEuroAsianMCEngine::simulate_geometric_path_with_average(
        Real S0, Real T, Real r, Real q, Real sigma,
        int num_dates,
        const IPayoff &payoff,
        Pcg32 &rng,
        AntitheticGaussianGenerator &gaussian_gen)
    {
        const Real dt = T / static_cast<Real>(num_dates);
        const Real sigma_sqrt_dt = sigma * std::sqrt(dt);
        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real exp_drift = std::exp(drift);

        Real S = S0;
        Real sum_log_S = 0.0;

        for (int j = 0; j < num_dates; ++j)
        {
            const Real z = gaussian_gen(rng);
            S = S * exp_drift * std::exp(sigma_sqrt_dt * z);
            sum_log_S += std::log(S);
        }

        // Geometric mean = exp(average of log prices)
        const Real log_geometric_mean = sum_log_S / static_cast<Real>(num_dates);
        const Real geometric_mean = std::exp(log_geometric_mean);
        const Real payoff_val = payoff(geometric_mean);

        return std::make_pair(payoff_val, geometric_mean);
    }

    void BSEuroAsianMCEngine::validate(const AsianOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("AsianOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("AsianOption.exercise is null");
        if (opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument("Non-European exercise is not supported by this engine");
        if (opt.exercise->dates().size() != 1)
            throw InvalidInput("Expected single maturity date for European Asian option");
    }

} // namespace quantModeling
