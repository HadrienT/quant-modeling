#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/utils/gaussian_source.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <algorithm>

namespace quantModeling
{

  namespace
  {
    /// Compile-time dispatch (payoff type × antithetic × IS) into the
    /// templated kernel — the only runtime branches left are per *request*,
    /// not per path.
    template <GaussianSource Source>
    mc::VanillaStats run_vanilla_kernel(const mc::VanillaTerminalSpec &spec,
                                        OptionType optType, bool antithetic,
                                        int n_paths, Source &gauss,
                                        Real is_shift = Real(0))
    {
      using mc::simulate_vanilla_terminal;
      const bool is = (is_shift != Real(0));
      if (optType == OptionType::Call)
      {
        if (is)
          return antithetic
                     ? simulate_vanilla_terminal<OptionType::Call, true, true>(spec, n_paths, gauss, is_shift)
                     : simulate_vanilla_terminal<OptionType::Call, false, true>(spec, n_paths, gauss, is_shift);
        return antithetic
                   ? simulate_vanilla_terminal<OptionType::Call, true>(spec, n_paths, gauss)
                   : simulate_vanilla_terminal<OptionType::Call, false>(spec, n_paths, gauss);
      }
      if (is)
        return antithetic
                   ? simulate_vanilla_terminal<OptionType::Put, true, true>(spec, n_paths, gauss, is_shift)
                   : simulate_vanilla_terminal<OptionType::Put, false, true>(spec, n_paths, gauss, is_shift);
      return antithetic
                 ? simulate_vanilla_terminal<OptionType::Put, true>(spec, n_paths, gauss)
                 : simulate_vanilla_terminal<OptionType::Put, false>(spec, n_paths, gauss);
    }
  } // namespace

  void BSEuroVanillaMCEngine::visit(const VanillaOption &opt)
  {
    validate(opt);
    const auto &m = require_model<ILocalVolModel>("BSEuroVanillaMCEngine");
    const PricingSettings &settings = ctx_.settings;

    const Real S0 = m.spot0();
    const Real r = m.rate_r();
    const Real q = m.yield_q();
    const Real v = m.vol_sigma();

    const Real T = opt.exercise->dates().front();
    const OptionType optType = opt.payoff->type();
    const Real K = opt.payoff->strike();

    // ---- Precompute all constants into the kernel spec
    const GreeksBumps bumps;
    const Real itoCorrection = -0.5 * v * v;
    const Real dS = S0 * bumps.delta_bump;
    const Real T_up = T + bumps.theta_bump;
    const Real T_dn = std::max(1e-8, T - bumps.theta_bump);

    mc::VanillaTerminalSpec spec;
    spec.S0 = S0;
    spec.K = K;
    spec.sigma = v;
    spec.T = T;
    spec.sqrtT = std::sqrt(T);
    spec.movedSpot = S0 * std::exp((r - q + itoCorrection) * T);
    spec.rootVariance = v * spec.sqrtT;
    spec.df = m.discount_curve().discount(T);
    spec.dS = dS;
    spec.factor_up = (S0 + dS) / S0;
    spec.factor_dn = (S0 - dS) / S0;
    spec.theta_bump = bumps.theta_bump;
    spec.movedSpot_upT = S0 * std::exp((r - q + itoCorrection) * T_up);
    spec.movedSpot_dnT = S0 * std::exp((r - q + itoCorrection) * T_dn);
    spec.rootVariance_upT = v * std::sqrt(T_up);
    spec.rootVariance_dnT = v * std::sqrt(T_dn);
    spec.df_upT = m.discount_curve().discount(T_up);
    spec.df_dnT = m.discount_curve().discount(T_dn);

    // ---- Run the paths
    const Real is_shift = settings.mc_importance_sampling
                              ? mc::optimal_is_shift(spec)
                              : Real(0);

    mc::VanillaStats stats;
    std::string diag;
    if (settings.mc_sampler == SamplerKind::Sobol ||
        settings.mc_sampler == SamplerKind::Stratified)
    {
      // Batched estimators (RQMC digital shifts / stratified jitter seeds):
      // draws within a batch are not i.i.d., but batch means are, so the
      // Welford accumulators over batch means give the grand mean and an
      // unbiased standard error. Antithetic is redundant here and ignored.
      const bool sobol = (settings.mc_sampler == SamplerKind::Sobol);
      const int B = std::max(2, settings.mc_rqmc_batches);
      const int per_batch = std::max(1, settings.mc_paths / B);
      for (int b = 0; b < B; ++b)
      {
        const uint64_t batch_seed =
            (static_cast<uint64_t>(static_cast<uint32_t>(settings.mc_seed)) << 32) |
            static_cast<uint64_t>(b);
        mc::VanillaStats batch;
        if (sobol)
        {
          SobolGaussianSource gauss(/*dimension=*/1, batch_seed);
          batch = run_vanilla_kernel(spec, optType, /*antithetic=*/false,
                                     per_batch, gauss, is_shift);
        }
        else
        {
          StratifiedGaussianSource gauss(Pcg32(batch_seed, 0x5717a7ull), per_batch);
          batch = run_vanilla_kernel(spec, optType, /*antithetic=*/false,
                                     per_batch, gauss, is_shift);
        }
        stats.payoff.add(batch.payoff.mean);
        stats.delta.add(batch.delta.mean);
        stats.vega.add(batch.vega.mean);
        stats.rho.add(batch.rho.mean);
        stats.gamma.add(batch.gamma.mean);
        stats.theta.add(batch.theta.mean);
      }
      diag = std::string("BS MC European vanilla (flat r,q,sigma) + ") +
             (sobol ? "Sobol RQMC" : "stratified sampling") + " (" +
             std::to_string(B) + " batches)";
    }
    else
    {
      RngFactory rngFact(static_cast<uint64_t>(settings.mc_seed));
      if (settings.mc_gaussian == GaussianKind::InverseNormal)
      {
        InverseNormalSource gauss(rngFact.make(0));
        stats = run_vanilla_kernel(spec, optType, settings.mc_antithetic,
                                   settings.mc_paths, gauss, is_shift);
      }
      else
      {
        BoxMullerSource gauss(rngFact.make(0));
        stats = run_vanilla_kernel(spec, optType, settings.mc_antithetic,
                                   settings.mc_paths, gauss, is_shift);
      }
      diag = settings.mc_antithetic
                 ? "BS MC European vanilla (flat r,q,sigma) + antithetic"
                 : "BS MC European vanilla (flat r,q,sigma)";
    }
    if (is_shift != Real(0))
      diag += " + importance sampling (drift shift to strike)";

    // ---- Assemble result
    const Real disc = spec.df;
    const Real N = opt.notional;

    PricingResult out;
    out.diagnostics = diag;
    out.npv = N * disc * stats.payoff.mean;
    out.mc_std_error = N * disc * stats.payoff.std_error();

    out.greeks.delta = N * stats.delta.mean;
    out.greeks.delta_std_error = N * stats.delta.std_error();

    out.greeks.vega = N * disc * stats.vega.mean;
    out.greeks.vega_std_error = N * disc * stats.vega.std_error();

    out.greeks.rho = N * disc * stats.rho.mean;
    out.greeks.rho_std_error = N * disc * stats.rho.std_error();

    out.greeks.gamma = N * stats.gamma.mean;
    out.greeks.gamma_std_error = N * stats.gamma.std_error();

    out.greeks.theta = N * stats.theta.mean;
    out.greeks.theta_std_error = N * stats.theta.std_error();

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

  void BSEuroVanillaMCEngine::visit(const BarrierOption &)
  {
    throw UnsupportedInstrument(
        "BSEuroVanillaMCEngine does not support barrier options. "
        "Use BSEuroBarrierMCEngine instead.");
  }

  void BSEuroVanillaMCEngine::visit(const DigitalOption &)
  {
    throw UnsupportedInstrument(
        "BSEuroVanillaMCEngine does not support digital options. "
        "Use BSDigitalAnalyticEngine instead.");
  }

}; // namespace quantModeling
