#ifndef MODELS_RATES_FLAT_RATE_HPP
#define MODELS_RATES_FLAT_RATE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/base.hpp"
#include <string>

namespace quantModeling
{

    struct IFlatRateModel : public IModel
    {
        virtual ~IFlatRateModel() = default;
        virtual Real rate() const = 0; // continuously compounded flat rate

        /// Discount curve for present-value computations.
        virtual const DiscountCurve &discount_curve() const = 0;
    };

    struct FlatRateModel final : public IFlatRateModel
    {
        Real r_;
        DiscountCurve disc_curve_;

        explicit FlatRateModel(Real r) : r_(r), disc_curve_(r) {}

        /// Construct with a custom (possibly non-flat) discount curve.
        FlatRateModel(Real r, DiscountCurve curve)
            : r_(r), disc_curve_(std::move(curve)) {}

        Real rate() const override { return r_; }
        const DiscountCurve &discount_curve() const override { return disc_curve_; }
        std::string model_name() const noexcept override { return "FlatRateModel"; }
    };

} // namespace quantModeling

#endif
