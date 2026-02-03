#include "quantModeling/engines/base.hpp"

namespace quantModeling {

EngineBase::EngineBase(PricingContext ctx) : ctx_(std::move(ctx)) {
  if (!ctx_.model)
    throw InvalidInput("PricingContext.model is null");
}

EngineBase::~EngineBase() = default;

const PricingResult &EngineBase::results() const { return res_; }

[[noreturn]] void EngineBase::unsupported(const char *instName) {
  throw UnsupportedInstrument(
      std::string("Engine does not support instrument: ") + instName);
}

} // namespace quantModeling