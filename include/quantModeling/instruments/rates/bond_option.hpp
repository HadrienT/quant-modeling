#ifndef INSTRUMENT_RATES_BOND_OPTION_HPP
#define INSTRUMENT_RATES_BOND_OPTION_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief European option on a zero-coupon bond.
     *
     * Payoff at T_option:
     *   Call: max(P(T_option, T_bond) − K, 0)
     *   Put:  max(K − P(T_option, T_bond), 0)
     *
     * Priced analytically under Vasicek and Hull-White, via Monte Carlo
     * for CIR and other short-rate models.
     */
    struct BondOption final : Instrument
    {
        Time option_maturity; ///< T — option expiry
        Time bond_maturity;   ///< S — underlying ZCB maturity (S > T)
        Real strike;          ///< K
        bool is_call;
        Real notional = 1.0;

        BondOption(Time option_mat, Time bond_mat, Real strike_,
                   bool is_call_, Real notional_ = 1.0)
            : option_maturity(option_mat), bond_maturity(bond_mat),
              strike(strike_), is_call(is_call_), notional(notional_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
