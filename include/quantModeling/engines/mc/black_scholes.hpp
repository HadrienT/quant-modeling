#ifndef ENGINE_MC_BS_HPP
#define ENGINE_MC_BS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"

namespace quantModeling {
class BSEuroVanillaMCEngine final : public EngineBase {
public:
  using EngineBase::EngineBase;
  void visit(const VanillaOption &opt) override;

private:
  static void validate(const VanillaOption &opt);
};
} // namespace quantModeling
#endif