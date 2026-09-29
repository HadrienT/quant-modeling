#ifndef ENGINE_ANALYTIC_CDS_HPP
#define ENGINE_ANALYTIC_CDS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/credit/cds.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/discount_curve.hpp"

namespace quantModeling
{

    /// The two legs of a CDS per unit notional, before the buyer/seller sign.
    struct CdsLegs
    {
        /// Risky PV01: Σ α_i DF(t_i) S(t_i) + the accrued-on-default term,
        /// i.e. the premium leg's value per unit of spread.
        Real risky_annuity = 0.0;
        /// Of which: the accrued-on-default part.
        Real accrued_on_default = 0.0;
        /// (1 - R) ∫ DF(t) dP(default by t).
        Real protection = 0.0;

        Real par_spread() const { return protection / risky_annuity; }
    };

    /**
     * @brief Value both legs of a CDS on a discount curve and a credit curve.
     *
     * The integrals are exact under the ISDA CDS Standard Model's
     * assumptions (O'Kane 2008, §6.6): the horizon is cut at every hazard
     * pillar, discount pillar and payment date, and on each piece both the
     * hazard rate λ and the forward rate f are constant, so
     *   ∫ λ e^{-(λ+f)s} ds = λ / (λ+f) · (1 - e^{-(λ+f)Δ})
     * and the accrued-on-default integral has the matching closed form (a
     * Taylor expansion takes over when λ+f is near zero). f on a piece is the
     * one implied by the discount factors at its ends — exact because
     * DiscountCurve interpolates log-linearly between its pillars.
     */
    CdsLegs cds_legs(const CreditDefaultSwap &cds, const DiscountCurve &discount,
                     const CreditCurve &credit, Real recovery);

    /// Value per the instrument's side: +(protection - spread·annuity)·notional
    /// for the buyer, the opposite for the seller.
    Real cds_npv(const CreditDefaultSwap &cds, const CdsLegs &legs);

    /**
     * @brief Prices a CreditDefaultSwap under an IIntensityModel. NPV only;
     *        the par spread and legs are in cds_legs().
     */
    class CdsAnalyticEngine final : public EngineBase
    {
      public:
        using EngineBase::EngineBase;

        void visit(const CreditDefaultSwap &cds) override;

        void visit(const VanillaOption &) override { unsupported("vanilla option"); }
        void visit(const AsianOption &) override { unsupported("Asian option"); }
        void visit(const BarrierOption &) override { unsupported("barrier option"); }
        void visit(const DigitalOption &) override { unsupported("digital option"); }
        void visit(const EquityFuture &) override { unsupported("equity future"); }
        void visit(const ZeroCouponBond &) override { unsupported("zero-coupon bond"); }
        void visit(const FixedRateBond &) override { unsupported("fixed-rate bond"); }
    };

} // namespace quantModeling

#endif
