#ifndef INSTRUMENT_EQUITY_AUTOCALL_HPP
#define INSTRUMENT_EQUITY_AUTOCALL_HPP

#include "quantModeling/instruments/base.hpp"
#include <vector>

namespace quantModeling
{

    /**
     * @brief Autocallable structured note (single underlying).
     *
     * Classic autocall mechanics:
     *  - At each observation date T_i, if S(T_i) ≥ autocall_barrier × S0,
     *    the note is called: the investor receives notional + coupon_i.
     *  - If the note survives to the final date T_n:
     *      - If S(T_n) ≥ coupon_barrier × S0, the investor receives
     *        notional + final coupon.
     *      - If S(T_n) < put_barrier × S0 (knock-in put triggered at any
     *        point, or only at final — controlled by ki_continuous),
     *        the investor receives notional × S(T_n) / S0
     *        (i.e. suffers the loss).
     *      - Otherwise, the investor receives notional (capital protected).
     *
     * Convention: all barriers are expressed as fractions of S0
     * (e.g. autocall_barrier = 1.0 means at-the-money).
     *
     * Coupons can be:
     *  - fixed: coupon_rate × (i+1) accumulated ("memory coupon") or
     *    coupon_rate per period — controlled by memory_coupon flag.
     *  - conditional on coupon_barrier: only paid if S(T_i) ≥ coupon_barrier × S0.
     */
    struct AutocallNote final : Instrument
    {
        std::vector<Time> observation_dates; ///< T_1, ..., T_n  (sorted, > 0)
        Real autocall_barrier;               ///< fraction of S0 for early redemption (e.g. 1.0)
        Real coupon_barrier;                 ///< fraction of S0 for coupon payment (e.g. 0.7)
        Real put_barrier;                    ///< fraction of S0 for knock-in put (e.g. 0.6)
        Real coupon_rate;                    ///< per-period coupon (fraction of notional)
        Real notional = 1000.0;
        bool memory_coupon = true;  ///< if true, missed coupons accumulate
        bool ki_continuous = false; ///< if true, KI put monitored continuously; else only at final

        AutocallNote(std::vector<Time> obs_dates,
                     Real ac_barrier,
                     Real cpn_barrier,
                     Real put_barr,
                     Real cpn_rate,
                     Real ntl = 1000.0,
                     bool mem_cpn = true,
                     bool ki_cont = false)
            : observation_dates(std::move(obs_dates)),
              autocall_barrier(ac_barrier),
              coupon_barrier(cpn_barrier),
              put_barrier(put_barr),
              coupon_rate(cpn_rate),
              notional(ntl),
              memory_coupon(mem_cpn),
              ki_continuous(ki_cont)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
