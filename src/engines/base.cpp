#include "quantModeling/engines/base.hpp"

#include "quantModeling/instruments/equity/vanilla.hpp"

namespace quantModeling
{

    EngineBase::EngineBase(PricingContext ctx)
        : ctx_(std::move(ctx))
    {
        if (!ctx_.model)
            throw InvalidInput("PricingContext.model is null");
    }

    EngineBase::~EngineBase() = default;

    const PricingResult &EngineBase::results() const
    {
        return res_;
    }

    [[noreturn]] void EngineBase::unsupported(const char *instName)
    {
        throw UnsupportedInstrument(
            std::string("Engine does not support instrument: ") + instName);
    }

    void validate_single_maturity_vanilla(const VanillaOption &opt, const char *european_only)
    {
        if (!opt.payoff)
            throw InvalidInput("VanillaOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("VanillaOption.exercise is null");
        if (european_only != nullptr && opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument(european_only);
        if (opt.exercise->dates().size() != 1)
            throw InvalidInput(european_only != nullptr
                                   ? "EuropeanExercise must contain exactly one date (maturity)"
                                   : "VanillaExercise must contain exactly one date (maturity)");
        const Real T = opt.exercise->dates().front();
        if (!(T > 0.0))
            throw InvalidInput("Maturity T must be > 0");
        if (!(opt.notional > 0.0))
            throw InvalidInput("Notional must be > 0");
        const Real K = opt.payoff->strike();
        if (!(K > 0.0))
            throw InvalidInput("Strike must be > 0");
    }

} // namespace quantModeling
