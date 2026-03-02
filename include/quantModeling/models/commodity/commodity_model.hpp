#ifndef MODELS_COMMODITY_COMMODITY_MODEL_HPP
#define MODELS_COMMODITY_COMMODITY_MODEL_HPP

#include "quantModeling/models/base.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    /**
     * @brief Interface for a single-commodity model.
     *
     * Cost-of-carry framework:
     *   F(0,T) = S0 × exp((r + storage − convenience_yield) × T)
     *
     * For Black '76 option pricing the vol() method returns the
     * volatility of the forward (assumed lognormal).
     */
    struct ICommodityModel : public IModel
    {
        virtual ~ICommodityModel() = default;
        virtual Real spot() const = 0;              ///< spot price
        virtual Real rate() const = 0;              ///< risk-free rate
        virtual Real storage_cost() const = 0;      ///< annualised storage cost
        virtual Real convenience_yield() const = 0; ///< annualised convenience yield
        virtual Real vol_sigma() const = 0;         ///< forward volatility

        /// Discount curve for present-value computations.
        virtual const DiscountCurve &discount_curve() const = 0;

        /// Cost-of-carry rate  r + u − y
        Real carry() const { return rate() + storage_cost() - convenience_yield(); }

        /// Forward price for maturity T
        Real forward(Time T) const { return spot() * std::exp(carry() * T); }
    };

    /**
     * @brief Concrete flat-vol commodity model (Black '76).
     */
    struct CommodityBlackModel final : public ICommodityModel
    {
        Real s0_;  ///< spot
        Real r_;   ///< risk-free rate
        Real u_;   ///< storage cost
        Real y_;   ///< convenience yield
        Real sig_; ///< forward vol

        CommodityBlackModel(Real s0, Real r, Real u, Real y, Real sigma)
            : s0_(s0), r_(r), u_(u), y_(y), sig_(sigma), disc_curve_(r) {}

        Real spot() const override { return s0_; }
        Real rate() const override { return r_; }
        Real storage_cost() const override { return u_; }
        Real convenience_yield() const override { return y_; }
        Real vol_sigma() const override { return sig_; }
        const DiscountCurve &discount_curve() const override { return disc_curve_; }
        std::string model_name() const noexcept override { return "CommodityBlackModel"; }

    private:
        DiscountCurve disc_curve_;
    };

} // namespace quantModeling

#endif
