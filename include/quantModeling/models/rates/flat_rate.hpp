#ifndef MODELS_RATES_FLAT_RATE_HPP
#define MODELS_RATES_FLAT_RATE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/base.hpp"
#include <string>

namespace quantModeling
{

    struct IFlatRateModel : public IModel
    {
        virtual ~IFlatRateModel() = default;
        virtual Real rate() const = 0; // continuously compounded flat rate
    };

    struct FlatRateModel final : public IFlatRateModel
    {
        Real r_;
        explicit FlatRateModel(Real r) : r_(r) {}

        Real rate() const override { return r_; }
        std::string model_name() const noexcept override { return "FlatRateModel"; }
    };

} // namespace quantModeling

#endif
