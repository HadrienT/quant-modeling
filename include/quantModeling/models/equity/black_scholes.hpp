#ifndef EQUITY_BLACK_SCHOLES_HPP
#define EQUITY_BLACK_SCHOLES_HPP

#include "quantModeling/models/base.hpp"
#include "quantModeling/core/types.hpp"
#include <string>
namespace quantModeling
{

    struct IBlackScholesModel : public IModel
    {
        virtual ~IBlackScholesModel() = default;
        virtual Real spot0() const = 0;
        virtual Real rate_r() const = 0;    // continuously compounded risk-free rate
        virtual Real yield_q() const = 0;   // continuously compounded dividend yield
        virtual Real vol_sigma() const = 0; // flat vol
    };

    struct BlackScholesModel final : public IBlackScholesModel
    {
        Real s0_, r_, q_, sigma_;
        BlackScholesModel(Real s0, Real r, Real q, Real sigma)
            : s0_(s0), r_(r), q_(q), sigma_(sigma) {}

        Real spot0() const override { return s0_; }
        Real rate_r() const override { return r_; }
        Real yield_q() const override { return q_; }
        Real vol_sigma() const override { return sigma_; }
        std::string model_name() const noexcept override { return "BlackScholesModel"; }
    };
}
#endif