#ifndef EQUITY_BLACK_SCHOLES_HPP
#define EQUITY_BLACK_SCHOLES_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <string>

namespace quantModeling
{

  /**
   * @brief Black-Scholes model with flat volatility
   *
   * Implements flat (constant) local volatility.
   * Can be used with analytic engines (European), tree engines, and PDE engines.
   */
  struct BlackScholesModel final : public ILocalVolModel
  {
    Real s0_, r_, q_, sigma_;

    BlackScholesModel(Real s0, Real r, Real q, Real sigma)
        : s0_(s0), r_(r), q_(q), sigma_(sigma) {}

    Real spot0() const override { return s0_; }
    Real rate_r() const override { return r_; }
    Real yield_q() const override { return q_; }
    Real vol_sigma() const override { return sigma_; }

    std::string model_name() const noexcept override
    {
      return "BlackScholesModel";
    }
  };

} // namespace quantModeling

#endif