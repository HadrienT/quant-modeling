#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/utils/gaussian_source.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"

namespace quantModeling
{

  namespace
  {
    /// Compile-time dispatch (payoff type × antithetic) into the templated
    /// kernel — the only runtime branches left are per *request*, not per path.
    template <GaussianSource Source>
    mc::VanillaStats run_vanilla_kernel(const mc::VanillaTerminalSpec &spec,
                                        OptionType optType, bool antithetic,
                                        int n_paths, Source &gauss)
    {
      using mc::simulate_vanilla_terminal;
      if (optType == OptionType::Call)
      {
        return antithetic
                   ? simulate_vanilla_terminal<OptionType::Call, true>(spec, n_paths, gauss)
                   : simulate_vanilla_terminal<OptionType::Call, false>(spec, n_paths, gauss);
      }
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

    // ---- Run the templated kernel with the selected Gaussian source
    RngFactory rngFact(static_cast<uint64_t>(settings.mc_seed));
    mc::VanillaStats stats;
    if (settings.mc_gaussian == GaussianKind::InverseNormal)
    {
      InverseNormalSource gauss(rngFact.make(0));
      stats = run_vanilla_kernel(spec, optType, settings.mc_antithetic,
                                 settings.mc_paths, gauss);
    }
    else
    {
      BoxMullerSource gauss(rngFact.make(0));
      stats = run_vanilla_kernel(spec, optType, settings.mc_antithetic,
                                 settings.mc_paths, gauss);
    }

    // ---- Assemble result
    const Real disc = spec.df;
    const Real N = opt.notional;

    PricingResult out;
    out.diagnostics = settings.mc_antithetic
                          ? "BS MC European vanilla (flat r,q,sigma) + antithetic"
                          : "BS MC European vanilla (flat r,q,sigma)";
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
