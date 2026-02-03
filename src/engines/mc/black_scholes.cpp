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
  const OptionType type = payoff.type();
  const Real K = payoff.strike();

  RngFactory rngFact(settings.mc_seed);
  Pcg32 rng = rngFact.make(0);
  NormalBoxMuller gaussianGenerator;
  Real sum = 0.0;
  Real itoCorrection = -0.5 * v * v;
  Real movedSpot = S0 * std::exp((r - q + itoCorrection) * T);

  for (int i = 0; i < settings.mc_paths; ++i) {
    Real thisGaussian = gaussianGenerator(rng);
    Real thisSpot = movedSpot * std::exp(rootVariance * thisGaussian);
    Real thisPayoff = payoff(thisSpot);
    sum += thisPayoff;
  }
  Real price = std::exp(-r * T) * sum / static_cast<Real>(settings.mc_paths);
  PricingResult out;
  out.diagnostics = "BS MC European vanilla (flat r,q,sigma)";
  out.npv = opt.notional * price;
  res_ = out;
};

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
