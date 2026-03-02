#ifndef INSTRUMENT_EQUITY_RAINBOW_HPP
#define INSTRUMENT_EQUITY_RAINBOW_HPP

#include "quantModeling/instruments/base.hpp"
#include <vector>

namespace quantModeling
{

    /**
     * @brief Worst-of option on a basket of assets.
     *
     * At maturity T the payoff is computed on the worst-performing asset:
     *
     *   perf_worst = min_i  S_i(T) / S_i(0)
     *
     *   Call:  notional × max(perf_worst − strike, 0)
     *   Put:   notional × max(strike − perf_worst, 0)
     *
     * The strike is expressed in performance terms (e.g. 1.0 = ATM).
     *
     * These options are short correlation: lower correlation increases the
     * chance that at least one asset underperforms, making worst-of puts
     * cheaper and worst-of calls more expensive to hedge.
     */
    struct WorstOfOption final : Instrument
    {
        Time maturity; ///< expiry T
        Real strike;   ///< performance strike K (e.g. 1.0 = ATM)
        bool is_call = true;
        Real notional = 100.0;

        WorstOfOption(Time T, Real K, bool call = true, Real ntl = 100.0)
            : maturity(T), strike(K), is_call(call), notional(ntl) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    /**
     * @brief Best-of option on a basket of assets.
     *
     * At maturity T the payoff is computed on the best-performing asset:
     *
     *   perf_best = max_i  S_i(T) / S_i(0)
     *
     *   Call:  notional × max(perf_best − strike, 0)
     *   Put:   notional × max(strike − perf_best, 0)
     *
     * Best-of options are long correlation: higher correlation means all
     * assets move together, reducing the "free pick" advantage.
     */
    struct BestOfOption final : Instrument
    {
        Time maturity; ///< expiry T
        Real strike;   ///< performance strike K (e.g. 1.0 = ATM)
        bool is_call = true;
        Real notional = 100.0;

        BestOfOption(Time T, Real K, bool call = true, Real ntl = 100.0)
            : maturity(T), strike(K), is_call(call), notional(ntl) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
