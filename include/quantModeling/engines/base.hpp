#ifndef ENGINE_BASE_HPP
#define ENGINE_BASE_HPP

#include "quantModeling/pricers/context.hpp"
#include "quantModeling/core/results.hpp"
#include "quantModeling/instruments/base.hpp"
#include <string>
#include <memory>
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
                throw InvalidInput(std::string(engineName) +
                                   " requires model interface: " + ctx_.model->model_name());
            return *p;
        }
        [[noreturn]] void unsupported(const char *instName);
    };

}
#endif