
#ifndef INSTRUMENT_EQUITY_VANILLA_HPP
#define INSTRUMENT_EQUITY_VANILLA_HPP

#include <cmath>
#include <memory>
#include <quantModeling/core/types.hpp>
#include <quantModeling/instruments/base.hpp>

namespace quantModeling {
struct IExercise;

struct VanillaOption final : Instrument {
  std::shared_ptr<const IPayoff> payoff;
  std::shared_ptr<const IExercise> exercise;
  Real notional = 1.0;

  VanillaOption(std::shared_ptr<const IPayoff> p,
                std::shared_ptr<const IExercise> e, Real n = 1.0)
      : payoff(std::move(p)), exercise(std::move(e)), notional(n) {}

  void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
};

struct PlainVanillaPayoff final : IPayoff {
  OptionType t_;
  Real K_;

  PlainVanillaPayoff(OptionType t, Real K) : t_(t), K_(K) {}

  OptionType type() const override { return t_; }
  Real strike() const override { return K_; }

  Real operator()(Real spot) const override {
    if (t_ == OptionType::Call)
      return std::max(spot - K_, 0.0);
    return std::max(K_ - spot, 0.0);
  }
};
} // namespace quantModeling

#endif