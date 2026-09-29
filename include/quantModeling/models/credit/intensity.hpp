#ifndef MODELS_CREDIT_INTENSITY_HPP
#define MODELS_CREDIT_INTENSITY_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/base.hpp"

#include <string>

namespace quantModeling
{

    /**
     * @brief Reduced-form (intensity) credit model: default is the first jump
     *        of a Poisson process with deterministic intensity λ(t),
     *        independent of interest rates (Jarrow & Turnbull 1995; Duffie &
     *        Singleton 1999). With independence, a defaultable cash flow is
     *        worth its riskless value times the survival probability — the
     *        assumption under which a CDS has closed-form legs.
     */
    struct IIntensityModel : public IModel
    {
        virtual const DiscountCurve &discount_curve() const = 0;
        virtual const CreditCurve &credit_curve() const = 0;
        /// Expected recovery as a fraction of notional.
        virtual Real recovery() const = 0;
    };

    struct IntensityModel final : public IIntensityModel
    {
        DiscountCurve discount;
        CreditCurve credit;
        Real recovery_rate;

        IntensityModel(DiscountCurve discount_, CreditCurve credit_, Real recovery_)
            : discount(std::move(discount_)), credit(std::move(credit_)), recovery_rate(recovery_)
        {
            if (!(recovery_rate >= 0.0 && recovery_rate < 1.0))
                throw InvalidInput("IntensityModel: recovery must be in [0, 1)");
        }

        const DiscountCurve &discount_curve() const override { return discount; }
        const CreditCurve &credit_curve() const override { return credit; }
        Real recovery() const override { return recovery_rate; }
        std::string model_name() const noexcept override { return "IntensityModel"; }
    };

} // namespace quantModeling

#endif
