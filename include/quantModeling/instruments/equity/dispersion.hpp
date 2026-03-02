#ifndef INSTRUMENT_EQUITY_DISPERSION_HPP
#define INSTRUMENT_EQUITY_DISPERSION_HPP

#include "quantModeling/instruments/base.hpp"
#include <vector>

namespace quantModeling
{

    /**
     * @brief Dispersion swap / trade.
     *
     * A dispersion trade goes long single-stock variance and short index
     * variance. The payoff at maturity is:
     *
     *   N × [ Σ_i w_i σ²_i,realised  −  σ²_index,realised  −  K_spread ]
     *
     * where w_i are the weights of each constituent in the index.
     * K_spread is the dispersion strike (fair-spread at inception).
     *
     * observation_dates: discrete monitoring times.
     * n_assets = weights.size() (engine receives a MultiAssetBSModel).
     */
    struct DispersionSwap final : Instrument
    {
        Time maturity;      ///< expiry
        Real strike_spread; ///< K_spread (annualised variance spread)
        Real notional = 100.0;
        std::vector<Real> weights;           ///< constituent weights (sum to 1)
        std::vector<Time> observation_dates; ///< monitoring schedule

        DispersionSwap(Time mat, Real K_spread,
                       std::vector<Real> w,
                       Real notional_ = 100.0,
                       std::vector<Time> obs = {})
            : maturity(mat), strike_spread(K_spread), notional(notional_),
              weights(std::move(w)), observation_dates(std::move(obs)) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
