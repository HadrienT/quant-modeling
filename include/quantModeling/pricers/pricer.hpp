#ifndef PRICERS_PRICER_HPP
#define PRICERS_PRICER_HPP
#include "quantModeling/core/results.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"

namespace quantModeling {

inline PricingResult price(const Instrument &inst, EngineBase &engine) {
  inst.accept(engine);
  return engine.results();
}
} // namespace quantModeling

#endif