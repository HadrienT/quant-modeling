#ifndef EQUITY_BLACK_SCHOLES_HPP
#define EQUITY_BLACK_SCHOLES_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/models/volatility.hpp"
#include <string>

namespace quantModeling
{

  /**
   * @brief Black-Scholes model with flat (constant) volatility.
   *
   * Implements ILocalVolModel.  The vol() method returns a FlatVol whose
   * value(S, t) always returns sigma_, so MC engines that call
   * vol().value(S, t) per time-step are numerically identical to the
   * old vol_sigma() path.
   */
  struct BlackScholesModel final : public ILocalVolModel
  {
    Real s0_, r_, q_, sigma_;

    BlackScholesModel(Real s0, Real r, Real q, Real sigma)
        : s0_(s0), r_(r), q_(q), sigma_(sigma), flat_vol_(sigma), disc_curve_(r) {}

    Real spot0() const override { return s0_; }
    Real rate_r() const override { return r_; }
    Real yield_q() const override { return q_; }
    Real vol_sigma() const override { return sigma_; }

    /// Returns the flat volatility surface: value(S, t) == sigma_ for all S, t.
    const IVolatility &vol() const override { return flat_vol_; }

    /// Flat discount curve built from the risk-free rate r.
    const DiscountCurve &discount_curve() const override { return disc_curve_; }

    std::string model_name() const noexcept override
    {
      return "BlackScholesModel";
    }

  private:
    FlatVol flat_vol_;
    DiscountCurve disc_curve_;
  };

} // namespace quantModeling

#endif