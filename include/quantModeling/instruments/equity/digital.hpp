#ifndef INSTRUMENT_EQUITY_DIGITAL_HPP
#define INSTRUMENT_EQUITY_DIGITAL_HPP

#include "quantModeling/instruments/base.hpp"
#include <memory>

namespace quantModeling
{
    // Payoff type selector — determines the formula used for pricing.
    //   CashOrNothing  : pays a fixed cash_amount if S_T is in-the-money
    //   AssetOrNothing : pays S_T itself if S_T is in-the-money
    enum class DigitalPayoffType
    {
        CashOrNothing,
        AssetOrNothing
    };

    struct DigitalOption final : Instrument
    {
        std::shared_ptr<const IPayoff> payoff; // call/put + strike
        std::shared_ptr<const IExercise> exercise;
        DigitalPayoffType payoff_type = DigitalPayoffType::CashOrNothing;
        Real cash_amount = 1.0; // used only when payoff_type == CashOrNothing
        Real notional = 1.0;

        DigitalOption(std::shared_ptr<const IPayoff> p,
                      std::shared_ptr<const IExercise> e,
                      DigitalPayoffType pt = DigitalPayoffType::CashOrNothing,
                      Real cash = 1.0,
                      Real n = 1.0)
            : payoff(std::move(p)), exercise(std::move(e)),
              payoff_type(pt), cash_amount(cash), notional(n)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
