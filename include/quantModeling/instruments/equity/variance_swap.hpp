#ifndef INSTRUMENT_EQUITY_VARIANCE_SWAP_HPP
#define INSTRUMENT_EQUITY_VARIANCE_SWAP_HPP

#include "quantModeling/instruments/base.hpp"
#include <vector>

namespace quantModeling
{

    /**
     * @brief Variance swap: pays N_var × (σ²_realized − K_var) at maturity.
     *
     * The strike K_var is quoted as an annualised variance (i.e. σ²).
     * Monitoring is discretely sampled at observation_dates.
     * If observation_dates is empty the engine may default to daily monitoring.
     */
    struct VarianceSwap final : Instrument
    {
        Time maturity;                       ///< expiry
        Real strike_var;                     ///< K_var (annualised variance)
        Real notional = 100.0;               ///< vega notional (in variance terms)
        std::vector<Time> observation_dates; ///< discrete monitoring schedule (optional)

        VarianceSwap(Time mat, Real K_var, Real notional_ = 100.0,
                     std::vector<Time> obs = {})
            : maturity(mat), strike_var(K_var), notional(notional_),
              observation_dates(std::move(obs)) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    /**
     * @brief Volatility swap: pays N_vol × (σ_realized − K_vol) at maturity.
     *
     * Unlike a variance swap, the payoff is linear in realised volatility.
     * No simple replication exists, so pricing is MC-only.
     */
    struct VolatilitySwap final : Instrument
    {
        Time maturity;                       ///< expiry
        Real strike_vol;                     ///< K_vol (annualised vol)
        Real notional = 100.0;               ///< vega notional
        std::vector<Time> observation_dates; ///< discrete monitoring schedule (optional)

        VolatilitySwap(Time mat, Real K_vol, Real notional_ = 100.0,
                       std::vector<Time> obs = {})
            : maturity(mat), strike_vol(K_vol), notional(notional_),
              observation_dates(std::move(obs)) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
