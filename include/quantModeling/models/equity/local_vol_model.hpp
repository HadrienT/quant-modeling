#ifndef EQUITY_LOCAL_VOL_MODEL_HPP
#define EQUITY_LOCAL_VOL_MODEL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/base.hpp"
#include <string>

namespace quantModeling
{

    /**
     * @brief Interface for local volatility equity models
     *
     * Provides the minimal interface needed for tree-based and PDE pricing methods.
     * Local vol models assume volatility is deterministic (function of spot and time).
     *
     * Can be implemented by:
     * - Black-Scholes model (flat vol)
     * - Local volatility surface models
     * - Heston model approximations (using moment-matching)
     */
    struct ILocalVolModel : public IModel
    {
        virtual ~ILocalVolModel() = default;
        virtual Real spot0() const = 0;     // Initial spot price
        virtual Real rate_r() const = 0;    // Risk-free rate (continuous)
        virtual Real yield_q() const = 0;   // Dividend yield (continuous)
        virtual Real vol_sigma() const = 0; // Local volatility at spot and t=0
    };

} // namespace quantModeling

#endif
