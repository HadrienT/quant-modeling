#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"

namespace quantModeling
{

  void BSEuroVanillaMCEngine::visit(const VanillaOption &opt)
  {
    validate(opt);
    const auto &m = require_model<ILocalVolModel>("BSEuroVanillaMCEngine");
    PricingSettings settings = ctx_.settings;

    const Real S0 = m.spot0();
    const Real r = m.rate_r();
    const Real q = m.yield_q();
    const Real v = m.vol_sigma();

    const Real T = opt.exercise->dates().front();
    const Real rootVariance = v * std::sqrt(T);
    const auto &payoff = *opt.payoff;
    const OptionType optType = payoff.type();
    const Real K = payoff.strike();

    RngFactory rngFact(settings.mc_seed);
    Pcg32 rng = rngFact.make(0);
    NormalBoxMuller gaussianGenerator;

    const Real itoCorrection = -0.5 * v * v;
    const Real movedSpot = S0 * std::exp((r - q + itoCorrection) * T);
    const Real df = std::exp(-r * T);

    // FD bumps for gamma/theta (common random numbers)
    const GreeksBumps bumps;
    const Real dS = S0 * bumps.delta_bump;
    const Real S0_up = S0 + dS;
    const Real S0_dn = S0 - dS;
    const Real T_up = T + bumps.theta_bump;
    const Real T_dn = std::max(1e-8, T - bumps.theta_bump);

    const Real rootVariance_upT = v * std::sqrt(T_up);
    const Real rootVariance_dnT = v * std::sqrt(T_dn);
    const Real movedSpot_upT = S0 * std::exp((r - q + itoCorrection) * T_up);
    const Real movedSpot_dnT = S0 * std::exp((r - q + itoCorrection) * T_dn);
    const Real df_upT = std::exp(-r * T_up);
    const Real df_dnT = std::exp(-r * T_dn);

    // Welford for payoff and pathwise delta
    Real meanPayoff = 0.0;
    Real meanDelta = 0.0;
    Real M2_payoff = 0.0;
    Real M2_delta = 0.0;
    // Welford for LRM estimators (vega and rho)
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
    // Means and M2 for FD gamma/theta (tracked with Welford helper)
    Real meanPayoff_Sup = 0.0;
    Real meanPayoff_Sdn = 0.0;
    Real meanPayoff_Tup = 0.0;
    Real meanPayoff_Tdn = 0.0;
    Real M2_payoff_Sup = 0.0;
    Real M2_payoff_Sdn = 0.0;
    Real M2_payoff_Tup = 0.0;
    Real M2_payoff_Tdn = 0.0;
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

    const Real sqrtT = std::sqrt(T);
    if (!settings.mc_antithetic)
    {
      // ---- MC standard
      for (int i = 0; i < settings.mc_paths; ++i)
      {
        const Real z = gaussianGenerator(rng);
        const Real ST = movedSpot * std::exp(rootVariance * z);
        const Real payoff_val = payoff(ST);

        // Pathwise delta: d(payoff)/dS × dS/dS0 × discount
        // For call: dPayoff/dS = 1{S>K}, dS/dS0 = S/S0
        // For put:  dPayoff/dS = -1{S<K}, dS/dS0 = S/S0
        Real delta_val = 0.0;
        if (optType == OptionType::Call && ST > K)
        {
          delta_val = df * (ST / S0);
        }
        else if (optType == OptionType::Put && ST < K)
        {
          delta_val = -df * (ST / S0);
        }

        // LRM score functions (using z)
        // score_sigma = (z^2 - 1) / sigma
        // score_r = z * sqrt(T) / sigma
        Real score_sigma = (z * z - 1.0) / v;
        Real score_r = (z * sqrtT) / v;

        const Real path_vega = payoff_val * score_sigma;
        const Real path_rho = -T * payoff_val + payoff_val * score_r;

        welford_push(payoff_val, delta_val);

        // FD gamma/theta payoffs using common random numbers
        const Real factor_up = S0_up / S0;
        const Real factor_dn = S0_dn / S0;
        const Real payoff_up = payoff(ST * factor_up);
        const Real payoff_dn = payoff(ST * factor_dn);
        const Real ST_Tup = movedSpot_upT * std::exp(rootVariance_upT * z);
        const Real ST_Tdn = movedSpot_dnT * std::exp(rootVariance_dnT * z);
        const Real payoff_Tup = payoff(ST_Tup);
        const Real payoff_Tdn = payoff(ST_Tdn);

        const Real gamma_path = df * (payoff_up - 2.0 * payoff_val + payoff_dn) / (dS * dS);
        const Real theta_path = (df_dnT * payoff_Tdn - df_upT * payoff_Tup) / (2.0 * bumps.theta_bump);

        welford_update(payoff_up, meanPayoff_Sup, M2_payoff_Sup, n);
        welford_update(payoff_dn, meanPayoff_Sdn, M2_payoff_Sdn, n);
        welford_update(payoff_Tup, meanPayoff_Tup, M2_payoff_Tup, n);
        welford_update(payoff_Tdn, meanPayoff_Tdn, M2_payoff_Tdn, n);

        welford_update(path_vega, meanVega, M2_vega, n);
        welford_update(path_rho, meanRho, M2_rho, n);
        welford_update(gamma_path, meanGamma, M2_gamma, n);
        welford_update(theta_path, meanTheta, M2_theta, n);
      }
    }
    else
    {
      // ---- MC antithetic
      const int nbPairs = settings.mc_paths / 2;
      const bool hasOdd = (settings.mc_paths % 2) != 0;

      for (int i = 0; i < nbPairs; ++i)
      {
        const Real z = gaussianGenerator(rng);
        const Real STp = movedSpot * std::exp(rootVariance * z);
        const Real STm = movedSpot * std::exp(rootVariance * (-z));

        // Payoff pair
        const Real payoff_p = payoff(STp);
        const Real payoff_m = payoff(STm);
        const Real y_payoff = 0.5 * (payoff_p + payoff_m);

        // Pathwise delta pair
        Real delta_p = 0.0, delta_m = 0.0;
        if (optType == OptionType::Call)
        {
          if (STp > K)
            delta_p = df * (STp / S0);
          if (STm > K)
            delta_m = df * (STm / S0);
        }
        else
        {
          if (STp < K)
            delta_p = -df * (STp / S0);
          if (STm < K)
            delta_m = -df * (STm / S0);
        }
        const Real y_delta = 0.5 * (delta_p + delta_m);

        // LRM scores for pair
        Real score_sigma_p = (z * z - 1.0) / v;
        Real score_sigma_m = ((-z) * (-z) - 1.0) / v; // same as score_sigma_p
        Real score_r_p = (z * sqrtT) / v;
        Real score_r_m = ((-z) * sqrtT) / v;

        const Real y_vega = 0.5 * (payoff_p * score_sigma_p + payoff_m * score_sigma_m);
        const Real y_rho = 0.5 * ((-T * payoff_p + payoff_p * score_r_p) + (-T * payoff_m + payoff_m * score_r_m));

        welford_push(y_payoff, y_delta);

        // FD gamma/theta payoffs (pair-averaged)
        const Real factor_up = S0_up / S0;
        const Real factor_dn = S0_dn / S0;
        const Real payoff_p_up = payoff(STp * factor_up);
        const Real payoff_m_up = payoff(STm * factor_up);
        const Real payoff_p_dn = payoff(STp * factor_dn);
        const Real payoff_m_dn = payoff(STm * factor_dn);
        const Real y_payoff_up = 0.5 * (payoff_p_up + payoff_m_up);
        const Real y_payoff_dn = 0.5 * (payoff_p_dn + payoff_m_dn);

        const Real STp_Tup = movedSpot_upT * std::exp(rootVariance_upT * z);
        const Real STm_Tup = movedSpot_upT * std::exp(rootVariance_upT * (-z));
        const Real STp_Tdn = movedSpot_dnT * std::exp(rootVariance_dnT * z);
        const Real STm_Tdn = movedSpot_dnT * std::exp(rootVariance_dnT * (-z));
        const Real y_payoff_Tup = 0.5 * (payoff(STp_Tup) + payoff(STm_Tup));
        const Real y_payoff_Tdn = 0.5 * (payoff(STp_Tdn) + payoff(STm_Tdn));

        const Real gamma_path = df * (y_payoff_up - 2.0 * y_payoff + y_payoff_dn) / (dS * dS);
        const Real theta_path = (df_dnT * y_payoff_Tdn - df_upT * y_payoff_Tup) / (2.0 * bumps.theta_bump);

        welford_update(y_payoff_up, meanPayoff_Sup, M2_payoff_Sup, n);
        welford_update(y_payoff_dn, meanPayoff_Sdn, M2_payoff_Sdn, n);
        welford_update(y_payoff_Tup, meanPayoff_Tup, M2_payoff_Tup, n);
        welford_update(y_payoff_Tdn, meanPayoff_Tdn, M2_payoff_Tdn, n);

        // Update vega/rho with the same helper
        welford_update(y_vega, meanVega, M2_vega, n);
        welford_update(y_rho, meanRho, M2_rho, n);
        welford_update(gamma_path, meanGamma, M2_gamma, n);
        welford_update(theta_path, meanTheta, M2_theta, n);
      }

      if (hasOdd)
      {
        const Real z = gaussianGenerator(rng);
        const Real ST = movedSpot * std::exp(rootVariance * z);
        const Real payoff_val = payoff(ST);

        Real delta_val = 0.0;
        if (optType == OptionType::Call && ST > K)
        {
          delta_val = df * (ST / S0);
        }
        else if (optType == OptionType::Put && ST < K)
        {
          delta_val = -df * (ST / S0);
        }

        Real score_sigma = (z * z - 1.0) / v;
        Real score_r = (z * sqrtT) / v;

        const Real path_vega = payoff_val * score_sigma;
        const Real path_rho = -T * payoff_val + payoff_val * score_r;

        welford_push(payoff_val, delta_val);

        // FD gamma/theta payoffs for odd path
        const Real factor_up = S0_up / S0;
        const Real factor_dn = S0_dn / S0;
        const Real payoff_up = payoff(ST * factor_up);
        const Real payoff_dn = payoff(ST * factor_dn);
        const Real ST_Tup = movedSpot_upT * std::exp(rootVariance_upT * z);
        const Real ST_Tdn = movedSpot_dnT * std::exp(rootVariance_dnT * z);
        const Real payoff_Tup = payoff(ST_Tup);
        const Real payoff_Tdn = payoff(ST_Tdn);

        const Real gamma_path = df * (payoff_up - 2.0 * payoff_val + payoff_dn) / (dS * dS);
        const Real theta_path = (df_dnT * payoff_Tdn - df_upT * payoff_Tup) / (2.0 * bumps.theta_bump);

        welford_update(payoff_up, meanPayoff_Sup, M2_payoff_Sup, n);
        welford_update(payoff_dn, meanPayoff_Sdn, M2_payoff_Sdn, n);
        welford_update(payoff_Tup, meanPayoff_Tup, M2_payoff_Tup, n);
        welford_update(payoff_Tdn, meanPayoff_Tdn, M2_payoff_Tdn, n);

        // Update vega/rho with the same helper
        welford_update(path_vega, meanVega, M2_vega, n);
        welford_update(path_rho, meanRho, M2_rho, n);
        welford_update(gamma_path, meanGamma, M2_gamma, n);
        welford_update(theta_path, meanTheta, M2_theta, n);
      }
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
                          ? "BS MC European vanilla (flat r,q,sigma) + antithetic"
                          : "BS MC European vanilla (flat r,q,sigma)";
    out.npv = opt.notional * price;
    out.mc_std_error = opt.notional * priceStdError;

    out.greeks.delta = opt.notional * meanDelta;
    // Delta standard error: use Welford stats accumulated in M2_delta
    Real deltaSampleVar = 0.0;
    Real deltaVarMean = 0.0;
    if (n > 1)
    {
      deltaSampleVar = M2_delta / static_cast<Real>(n - 1);
      deltaVarMean = deltaSampleVar / static_cast<Real>(n);
    }
    const Real deltaStdError = (n > 1) ? std::sqrt(deltaVarMean) * opt.notional : 0.0;
    out.greeks.delta_std_error = deltaStdError;

    // Vega: discount factor applied to expectation of payoff*score_sigma
    const Real vegaStdErr = (n > 1) ? std::sqrt((M2_vega / static_cast<Real>(n - 1)) / static_cast<Real>(n)) : 0.0;
    out.greeks.vega = opt.notional * disc * meanVega;
    out.greeks.vega_std_error = opt.notional * disc * vegaStdErr;

    // Rho: estimator combined (-T * payoff + payoff * score_r)
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

  void BSEuroVanillaMCEngine::validate(const VanillaOption &opt)
  {
    if (!opt.payoff)
      throw InvalidInput("VanillaOption.payoff is null");
    if (!opt.exercise)
      throw InvalidInput("VanillaOption.exercise is null");
    if (opt.exercise->type() != ExerciseType::European)
    {
      throw UnsupportedInstrument(
          "Non-European exercise is not supported by this engine");
    }
    if (opt.exercise->dates().size() != 1)
    {
      throw InvalidInput(
          "EuropeanExercise must contain exactly one date (maturity)");
    }
    const Real T = opt.exercise->dates().front();
    if (!(T > 0.0))
      throw InvalidInput("Maturity T must be > 0");
    if (!(opt.notional > 0.0))
      throw InvalidInput("Notional must be > 0");
    const Real K = opt.payoff->strike();
    if (!(K > 0.0))
      throw InvalidInput("Strike must be > 0");
  }

  void BSEuroVanillaMCEngine::visit(const AsianOption &)
  {
    throw UnsupportedInstrument(
        "BSEuroVanillaMCEngine does not support Asian options. "
        "Use BSEuroAsianMCEngine instead.");
  }

  void BSEuroVanillaMCEngine::visit(const EquityFuture &)
  {
    throw UnsupportedInstrument("BSEuroVanillaMCEngine does not support equity futures.");
  }

  void BSEuroVanillaMCEngine::visit(const ZeroCouponBond &)
  {
    throw UnsupportedInstrument("BSEuroVanillaMCEngine does not support bonds.");
  }

  void BSEuroVanillaMCEngine::visit(const FixedRateBond &)
  {
    throw UnsupportedInstrument("BSEuroVanillaMCEngine does not support bonds.");
  }

}; // namespace quantModeling
