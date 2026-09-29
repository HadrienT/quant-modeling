#ifndef INSTRUMENT_CREDIT_CDS_HPP
#define INSTRUMENT_CREDIT_CDS_HPP

#include "quantModeling/instruments/base.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief A single-name credit default swap, seen from the protection
     *        buyer (O'Kane 2008, ch. 5).
     *
     * Premium leg: the buyer pays `spread · accruals[i] · notional` at each
     * `payment_times[i]` while the reference entity survives, plus the
     * premium accrued since the last payment date if it defaults in between.
     * Protection leg: on default the seller pays `(1 - R) · notional`. The
     * recovery rate R is a market assumption and lives with the credit curve
     * (IIntensityModel), not here: the contract does not fix it.
     *
     * Times are year fractions from the valuation date, the accrual start of
     * the first period being 0 (a contract already running is out of scope).
     */
    struct CreditDefaultSwap final : Instrument
    {
        Real spread;                     ///< running coupon, decimal (0.01 = 100bp)
        std::vector<Time> payment_times; ///< strictly increasing, last = maturity
        std::vector<Real> accruals;      ///< day-count fraction of each period
        Real notional = 1.0;
        bool protection_buyer = true;

        Time maturity() const { return payment_times.back(); }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    /// A CDS paying `frequency` times a year, with a short first period when
    /// the maturity is not a whole number of periods (accruals = period
    /// lengths in years, i.e. an ACT/ACT-like basis on the Time axis).
    CreditDefaultSwap make_cds(Time maturity, Real spread, int frequency = 4,
                               Real notional = 1.0, bool protection_buyer = true);

} // namespace quantModeling

#endif
