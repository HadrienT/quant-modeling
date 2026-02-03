#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/utils/rng.hpp"

namespace quantModeling {

void BSEuroVanillaMCEngine::visit(const VanillaOption &opt) {
  validate(opt);
  const auto &m = require_model<IBlackScholesModel>("BSEuroVanillaMCEngine");
  PricingSettings settings = ctx_.settings;

  const Real S0 = m.spot0();
  const Real r = m.rate_r();
  const Real q = m.yield_q();
  const Real v = m.vol_sigma();

  const Real T = opt.exercise->dates().front();
  const Real rootVariance = v * std::sqrt(T);
  const auto &payoff = *opt.payoff;

  RngFactory rngFact(settings.mc_seed);
  Pcg32 rng = rngFact.make(0);
  NormalBoxMuller gaussianGenerator;

  const Real itoCorrection = -0.5 * v * v;
  const Real movedSpot = S0 * std::exp((r - q + itoCorrection) * T);

  // Welford
  Real meanPayoff = 0.0;
  Real M2 = 0.0;
  int n = 0;

  auto welford_push = [&](Real x) {
    ++n;
    const Real delta = x - meanPayoff;
    meanPayoff += delta / static_cast<Real>(n);
    const Real delta2 = x - meanPayoff;
    M2 += delta * delta2;
  };

  if (!settings.mc_antithetic) {
    // ---- MC standard : n = mc_paths
    for (int i = 0; i < settings.mc_paths; ++i) {
      const Real z = gaussianGenerator(rng);
      const Real ST = movedSpot * std::exp(rootVariance * z);
      welford_push(payoff(ST));
    }
  } else {
    // ---- MC antithetic : on accumule y = 0.5*(payoff(z)+payoff(-z))
    const int nbPairs = settings.mc_paths / 2;
    const bool hasOdd = (settings.mc_paths % 2) != 0;

    for (int i = 0; i < nbPairs; ++i) {
      const Real z = gaussianGenerator(rng);
      const Real STp = movedSpot * std::exp(rootVariance * z);
      const Real STm = movedSpot * std::exp(rootVariance * (-z));
      const Real y = 0.5 * (payoff(STp) + payoff(STm));
      welford_push(y);
    }

    // optionnel : consommer le dernier path si mc_paths impair
    if (hasOdd) {
      const Real z = gaussianGenerator(rng);
      const Real ST = movedSpot * std::exp(rootVariance * z);
      welford_push(payoff(ST));
    }
  }

  Real sampleVariance = 0.0;
  Real varMean = 0.0;
  if (n > 1) {
    sampleVariance = M2 / static_cast<Real>(n - 1);
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
  res_ = out;
}

void BSEuroVanillaMCEngine::validate(const VanillaOption &opt) {
  if (!opt.payoff)
    throw InvalidInput("VanillaOption.payoff is null");
  if (!opt.exercise)
    throw InvalidInput("VanillaOption.exercise is null");
  if (opt.exercise->type() != ExerciseType::European) {
    throw UnsupportedInstrument(
        "Non-European exercise is not supported by this engine");
  }
  if (opt.exercise->dates().size() != 1) {
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
}; // namespace quantModeling
