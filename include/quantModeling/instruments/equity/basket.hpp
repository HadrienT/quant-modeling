#ifndef INSTRUMENT_EQUITY_BASKET_HPP
#define INSTRUMENT_EQUITY_BASKET_HPP

#include "quantModeling/instruments/base.hpp"
#include <memory>
#include <vector>

namespace quantModeling
{

    /**
     * @brief European basket option.
     *
     * Payoff (call): max( sum_i(w_i * S_i(T)) - K, 0 )
     * Payoff (put):  max( K - sum_i(w_i * S_i(T)), 0 )
     *
     * The strike K and option type (call/put) are carried by @c payoff.
     * weights must satisfy sum(w_i) = 1 (not enforced here; validated in the engine).
     */
    struct BasketOption final : Instrument
    {
        std::shared_ptr<const IPayoff> payoff; ///< provides strike() and type()
        std::shared_ptr<const IExercise> exercise;
        std::vector<Real> weights; ///< per-asset mixing weights
        Real notional = 1.0;

        BasketOption(std::shared_ptr<const IPayoff> p,
                     std::shared_ptr<const IExercise> e,
                     std::vector<Real> w,
                     Real n = 1.0)
            : payoff(std::move(p)), exercise(std::move(e)),
              weights(std::move(w)), notional(n)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
