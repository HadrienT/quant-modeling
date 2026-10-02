#ifndef QM_RISK_COLLATERAL_PATH_HPP
#define QM_RISK_COLLATERAL_PATH_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/csa.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

/**
 * @file collateral_path.hpp
 * @brief The collateral model of risk/collateral.hpp on **one path**, written
 *        once for the host and for the device (blueprint/wp/23-xva.md §14.11,
 *        lot X7).
 *
 * collateralise() runs it on a stored cube; the GPU exposure engine runs it
 * inside the kernel that has just valued the path, so that the cube never
 * has to exist (src/gpu/exposure.cu). Both call the functions below: there is
 * one statement of what a margin call is.
 */

namespace quantModeling
{

    /// The amounts of a CSA, without its margin period of risk (which is in
    /// the plan's dates).
    struct CollateralTerms
    {
        Real threshold_counterparty = 0.0;
        Real threshold_bank = 0.0;
        Real minimum_transfer_amount = 0.0;
        Real rounding = 0.0;
        Real independent_amount = 0.0;

        static CollateralTerms of(const Csa &csa)
        {
            return {csa.threshold_counterparty, csa.threshold_bank, csa.minimum_transfer_amount,
                    csa.rounding, csa.independent_amount};
        }
    };

    /// What happens to the trades' own cash flows during the margin period of
    /// risk (Andersen, Pykhtin & Sokol 2017, "Rethinking the margin period of
    /// risk").
    enum class MarginPeriodCashflows
    {
        /// "Classical-": both parties keep paying the trade flows up to the
        /// close-out. A flow the bank pays inside the period makes the value
        /// jump up while the collateral still reflects the value before it:
        /// exposure spikes on the bank's payment dates.
        Paid,
        /// "Classical+": both parties stop every payment together with the
        /// margin calls. The flows of the period are not exchanged and stay
        /// in the close-out amount: no spike.
        Withheld,
        /// The adverse case of the paper's full model: the bank keeps paying,
        /// the counterparty does not. The bank's spikes of `Paid`, plus what
        /// the counterparty owed and did not pay. (The full model also gives
        /// each party its own stop date; that refinement is not coded.)
        OnlyBankPays
    };

    /// max(V - H_C, 0) - max(-V - H_I, 0): what a perfect CSA would hold.
    QM_HOST_DEVICE inline Real variation_margin_required(Real value, const CollateralTerms &terms)
    {
        const Real called = value - terms.threshold_counterparty;
        const Real posted = -value - terms.threshold_bank;
        return (called > 0.0 ? called : 0.0) - (posted > 0.0 ? posted : 0.0);
    }

    /// One margin call: nothing moves below the minimum transfer amount,
    /// otherwise the transfer is rounded to the nearest multiple of the
    /// rounding.
    QM_HOST_DEVICE inline Real balance_after_call(Real held, Real required, const CollateralTerms &terms)
    {
        using std::fabs;
        using std::round;
        Real transfer = required - held;
        // Strictly below the MTA: no transfer. A zero transfer is none either.
        if (transfer == 0.0 || fabs(transfer) < terms.minimum_transfer_amount)
            return held;
        if (terms.rounding > 0.0)
            transfer = round(transfer / terms.rounding) * terms.rounding;
        return held + transfer;
    }

    /// Value net of initial margin on both sides: above the margin received
    /// for the bank's exposure, below minus the margin posted for the
    /// counterparty's, zero in between.
    QM_HOST_DEVICE inline Real net_of_initial_margin(Real value, Real received, Real posted)
    {
        if (value > received)
            return value - received;
        if (value < -posted)
            return value + posted;
        return 0.0;
    }

    /**
     * @brief The dates of a collateralised netting set: which dates report
     *        an exposure, which earlier date each one's collateral was called
     *        on, and what was held today.
     *
     * Plain arrays of the grid's size: the device reads them as they are.
     */
    struct CollateralPlan
    {
        /// lagged[i]: the date t_i - MPoR is today or earlier.
        static constexpr int kToday = -1;
        /// lagged[i]: t_i - MPoR is not on the grid; t_i reports nothing.
        static constexpr int kMissing = -2;

        CollateralTerms terms;
        MarginPeriodCashflows cashflows = MarginPeriodCashflows::Paid;
        /// Per grid date: the index of t - MPoR.
        std::vector<int> lagged;
        /// Per grid date: a margin call is observed there.
        std::vector<unsigned char> is_call_date;
        /// The reporting dates, as indices into the grid.
        std::vector<int> reporting;
        /// The balance after the call on today's value.
        Real held_today = 0.0;

        bool needs_cashflows() const { return cashflows != MarginPeriodCashflows::Paid; }
    };

    /// The same, as the pointers a kernel takes by value.
    struct CollateralPlanView
    {
        CollateralTerms terms;
        int cashflows = 0;
        const int *lagged = nullptr;
        const unsigned char *is_call_date = nullptr;
        const int *reporting = nullptr;
        int dates = 0;
        int reporting_dates = 0;
        Real held_today = 0.0;
    };

    inline CollateralPlanView view(const CollateralPlan &plan)
    {
        return {plan.terms,
                static_cast<int>(plan.cashflows),
                plan.lagged.data(),
                plan.is_call_date.data(),
                plan.reporting.data(),
                static_cast<int>(plan.lagged.size()),
                static_cast<int>(plan.reporting.size()),
                plan.held_today};
    }

    /**
     * @brief The collateral balance of one path at every call date: `held[i]`
     *        is written where `is_call_date[i]`, left alone elsewhere.
     *
     * The balance is only updated when a call is observed, in time order: the
     * minimum transfer amount makes it depend on the whole path.
     */
    QM_HOST_DEVICE inline void collateral_balances(const CollateralPlanView &plan, const Real *value,
                                                   Real *held)
    {
        Real balance = plan.held_today;
        for (int i = 0; i < plan.dates; ++i)
        {
            if (!plan.is_call_date[i])
                continue;
            balance = balance_after_call(balance, variation_margin_required(value[i], plan.terms),
                                         plan.terms);
            held[i] = balance;
        }
    }

    /**
     * @brief V(t) - C(t - MPoR) at reporting date r of one path, before any
     *        initial margin: the value, minus the collateral called at the
     *        lagged date and the independent amount, plus the flows of the
     *        margin period that were not exchanged.
     *
     * @param flow the netted cash flows of the path; read only when the plan
     *             needs them.
     */
    QM_HOST_DEVICE inline Real collateralised_value(const CollateralPlanView &plan, int r,
                                                    const Real *value, const Real *flow,
                                                    const Real *held)
    {
        const int i = plan.reporting[r];
        const int lagged = plan.lagged[i];
        const Real collateral = (lagged == CollateralPlan::kToday ? plan.held_today : held[lagged]) +
                                plan.terms.independent_amount;
        Real exposure = value[i] - collateral;
        if (plan.cashflows != static_cast<int>(MarginPeriodCashflows::Paid))
        {
            // Flows due in (t - MPoR, t] that were not exchanged stay in the
            // close-out amount.
            const int first = lagged == CollateralPlan::kToday ? 0 : lagged + 1;
            for (int j = first; j <= i; ++j)
                exposure += plan.cashflows == static_cast<int>(MarginPeriodCashflows::Withheld)
                                ? flow[j]
                                : (flow[j] > 0.0 ? flow[j] : 0.0);
        }
        return exposure;
    }

} // namespace quantModeling

#endif // QM_RISK_COLLATERAL_PATH_HPP
