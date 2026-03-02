#ifndef INSTRUMENT_RATES_CAPFLOOR_HPP
#define INSTRUMENT_RATES_CAPFLOOR_HPP

#include "quantModeling/instruments/base.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief Interest rate cap or floor.
     *
     * A cap (floor) is a portfolio of caplets (floorlets).
     *
     * Caplet on [T_i, T_{i+1}] with strike K:
     *   Payoff at T_{i+1} = N · δ_i · max(L(T_i, T_{i+1}) − K, 0)
     *
     * In a short-rate model framework, each caplet is equivalent to
     *   (1 + K δ_i) · Put on ZCB(T_{i+1}) with strike 1/(1 + K δ_i), expiry T_i
     *
     * Similarly, each floorlet is
     *   (1 + K δ_i) · Call on ZCB(T_{i+1}) with strike 1/(1 + K δ_i), expiry T_i
     */
    struct CapFloor final : Instrument
    {
        /// Tenor schedule [T_0, T_1, ..., T_n].
        /// Caplets cover intervals [T_0,T_1], [T_1,T_2], ..., [T_{n-1},T_n].
        std::vector<Time> schedule;

        Real strike; ///< Cap/floor rate K
        bool is_cap; ///< true = cap, false = floor
        Real notional = 1.0;

        CapFloor(std::vector<Time> schedule_, Real strike_,
                 bool is_cap_, Real notional_ = 1.0)
            : schedule(std::move(schedule_)), strike(strike_),
              is_cap(is_cap_), notional(notional_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
