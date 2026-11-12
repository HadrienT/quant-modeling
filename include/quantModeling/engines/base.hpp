#ifndef ENGINE_BASE_HPP
#define ENGINE_BASE_HPP

#include "quantModeling/core/results.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/pricers/context.hpp"
#include <memory>
#include <string>
namespace quantModeling
{

    class EngineBase : public IInstrumentVisitor
    {
      public:
        explicit EngineBase(PricingContext ctx);
        virtual ~EngineBase();

        const PricingResult &results() const;

      protected:
        PricingContext ctx_;
        PricingResult res_;

        template <class ModelIface>
        const ModelIface &require_model(const char *engineName) const
        {
            static_assert(std::is_base_of_v<IModel, ModelIface>,
                          "ModelIface must derive from IModel");

            const auto *p = dynamic_cast<const ModelIface *>(ctx_.model.get());
            if (!p)
                throw InvalidInput(
                    std::string(engineName) +
                    " requires model interface: " + ctx_.model->model_name());
            return *p;
        }
    };

    struct VanillaOption;

    /// The checks of an engine that prices a vanilla at its one maturity:
    /// payoff and exercise set, one date, positive maturity, notional and
    /// strike. With `european_only`, any other exercise type is refused with
    /// that message (UnsupportedInstrument).
    void validate_single_maturity_vanilla(const VanillaOption &opt, const char *european_only = nullptr);

} // namespace quantModeling
#endif