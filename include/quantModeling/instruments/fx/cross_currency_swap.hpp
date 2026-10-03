#ifndef INSTRUMENT_FX_CROSS_CURRENCY_SWAP_HPP
#define INSTRUMENT_FX_CROSS_CURRENCY_SWAP_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief Fixed-for-fixed cross-currency swap, starting today: the two
     *        parties exchange notionals at the spot rate now, pay each other
     *        fixed coupons in the currency they received, and exchange the
     *        notionals back at maturity.
     *
     * The initial exchange is made today at the market rate: it is worth
     * nothing and is behind the trade. What remains are two streams of
     * known cash flows, one in each currency — coupons and the notional at
     * maturity:
     *
     *   PV = ± [ S0 Σ_j c_f,j P_f(0, T_j) − Σ_j c_d,j P_d(0, T_j) ]
     *
     * with S0 the spot (domestic per foreign). Its exposure is that of the
     * final exchange: the notional is not amortised, so the risk on the
     * exchange rate grows until maturity (Gregory, The xVA Challenge, 4th
     * ed., the cross-currency swap profile of the exposure chapter).
     *
     * The one product added by lot X10 of blueprint/wp/23-xva.md, against
     * the roadmap's rule, by decision of the maintainer: a two-currency
     * exposure has no other textbook example.
     */
    struct CrossCurrencySwap final : Instrument
    {
        Time maturity;          ///< final exchange of notionals
        Real foreign_notional;  ///< in foreign currency
        Real domestic_notional; ///< in domestic currency
        Real foreign_rate;      ///< fixed coupon rate on the foreign notional
        Real domestic_rate;     ///< fixed coupon rate on the domestic notional
        int frequency = 1;      ///< coupons per year, both legs
        /// true: the bank receives the foreign leg and pays the domestic one.
        bool receive_foreign = true;

        CrossCurrencySwap(Time T, Real foreign_notional_, Real domestic_notional_, Real foreign_rate_,
                          Real domestic_rate_, int frequency_ = 1, bool receive_foreign_ = true)
            : maturity(T), foreign_notional(foreign_notional_), domestic_notional(domestic_notional_), foreign_rate(foreign_rate_), domestic_rate(domestic_rate_), frequency(frequency_), receive_foreign(receive_foreign_)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
