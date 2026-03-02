#ifndef INSTRUMENT_EQUITY_MOUNTAIN_HPP
#define INSTRUMENT_EQUITY_MOUNTAIN_HPP

#include "quantModeling/instruments/base.hpp"
#include <vector>

namespace quantModeling
{

    /**
     * @brief Himalaya (Mountain) option — multi-asset path-dependent exotic.
     *
     * At each observation date T_i (i = 1, ..., n):
     *   1. Compute the return R_j = S_j(T_i) / S_j(0) − 1 for every
     *      asset j still in the active basket.
     *   2. Lock in the best return: perf_i = max_j R_j.
     *   3. Remove that best-performing asset from the basket.
     *
     * At maturity T_n the average of locked-in returns is computed:
     *   avg_perf = (1/n) × Σ perf_i
     *
     * Payoff (call):  notional × max(avg_perf − strike, 0)
     * Payoff (put):   notional × max(strike − avg_perf, 0)
     *
     * The number of observation dates must equal the number of assets
     * (each date removes one asset).
     *
     * Pricing is Monte Carlo only — no analytic solution exists.
     */
    struct MountainOption final : Instrument
    {
        std::vector<Time> observation_dates; ///< T_1, ..., T_n  (one per asset)
        Real strike;                         ///< strike on average return (e.g. 0.0 for ATMF)
        bool is_call = true;
        Real notional = 100.0;

        MountainOption(std::vector<Time> obs_dates,
                       Real K,
                       bool call = true,
                       Real ntl = 100.0)
            : observation_dates(std::move(obs_dates)),
              strike(K),
              is_call(call),
              notional(ntl)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
