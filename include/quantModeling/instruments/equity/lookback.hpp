#ifndef INSTRUMENT_EQUITY_LOOKBACK_HPP
#define INSTRUMENT_EQUITY_LOOKBACK_HPP

#include "quantModeling/instruments/base.hpp"
#include <memory>

namespace quantModeling
{

    enum class LookbackStyle
    {
        FixedStrike,
        FloatingStrike
    };

    enum class LookbackExtremum
    {
        Minimum,
        Maximum
    };

    struct LookbackOption final : Instrument
    {
        std::shared_ptr<const IPayoff> payoff;
        std::shared_ptr<const IExercise> exercise;
        LookbackStyle style = LookbackStyle::FixedStrike;
        LookbackExtremum extremum = LookbackExtremum::Maximum;
        Real notional = 1.0;
        int n_steps = 0; // 0 => auto (252 * T)

        LookbackOption(std::shared_ptr<const IPayoff> p,
                       std::shared_ptr<const IExercise> e,
                       LookbackStyle s = LookbackStyle::FixedStrike,
                       LookbackExtremum ex = LookbackExtremum::Maximum,
                       Real n = 1.0)
            : payoff(std::move(p)), exercise(std::move(e)), style(s), extremum(ex), notional(n)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
