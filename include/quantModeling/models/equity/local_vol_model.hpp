#ifndef EQUITY_LOCAL_VOL_MODEL_HPP
#define EQUITY_LOCAL_VOL_MODEL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/base.hpp"
#include "quantModeling/models/volatility.hpp"
#include <string>

namespace quantModeling
{

    /**
     * @brief Interface for local volatility equity models.
     *
     * Provides the minimal interface needed for tree-based, PDE, and
     * Monte-Carlo pricing methods.  Local-vol models assume volatility is
     * deterministic — a function of spot and time.
     *
     * Can be implemented by:
     * - BlackScholesModel  (FlatVol — ignores S and t)
     * - DupireModel / GridLocalVolModel  (GridLocalVol — bilinear surface)
     * - Heston model approximations (via moment-matching)
     *
     * ## Volatility abstraction
     * MC engines that time-step the spot process should call
     *   @code  m.vol().value(S_current, t_current)  @endcode
     * at every step so they work uniformly for flat and local-vol models.
     *
     * Analytic and tree engines that only need a scalar vol (e.g. BSM
     * closed-form, finite-difference grids with a single constant sigma)
     * may continue to call vol_sigma() for backward compatibility.
     */
    struct ILocalVolModel : public IModel
    {
        virtual ~ILocalVolModel() = default;

        virtual Real spot0() const = 0;   ///< Initial spot price
        virtual Real rate_r() const = 0;  ///< Risk-free rate (continuous, annualised)
        virtual Real yield_q() const = 0; ///< Dividend yield (continuous, annualised)

        /**
         * @brief Representative scalar volatility for analytic/tree engines.
         *
         * For flat-vol models this is the constant sigma.
         * For surface models this may return a representative ATM value.
         * MC engines should prefer vol().value(S, t) instead.
         */
        virtual Real vol_sigma() const = 0;

        /**
         * @brief Full volatility surface σ(S, t) as an IVolatility object.
         *
         * Returned by const reference — the object is owned by the model and
         * remains valid for the lifetime of the model instance.
         *
         * FlatVol::value(S, t) always returns the constant sigma, so existing
         * flat-vol paths are numerically identical after the upgrade.
         */
        virtual const IVolatility &vol() const = 0;
    };

} // namespace quantModeling

#endif
