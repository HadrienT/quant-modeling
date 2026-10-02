#ifndef QM_ENGINES_XVA_XVA_RISKS_HPP
#define QM_ENGINES_XVA_XVA_RISKS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/collateral_path.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

/**
 * @file xva_risks.hpp
 * @brief The sensitivities of CVA, DVA and FVA of a netting set to every
 *        input of the model, by adjoint differentiation
 *        (blueprint/wp/23-xva.md §13.5 and §14.13, lot X8).
 *
 * The algorithm is the check-pointed adjoint Monte-Carlo of Savine (WP 17
 * §7), with the exposure engine in the place of the model and the product:
 *
 *  1. the inputs are leaves of the tape — the zero rate of each pillar of
 *     the curve, the mean reversion and the volatility of Hull-White, the
 *     hazard rates of both parties, the losses given default, the funding
 *     spreads;
 *  2. **before the mark**, once: everything the simulation is set up with is
 *     recorded as a function of them — the transitions of the state, the
 *     discount weights, every coefficient of every trade
 *     (engines/xva/exposure_program.hpp), the default probabilities;
 *  3. **after the mark**, path by path: the state, the value of each trade
 *     at each date, the netting, the collateral, and the path's own CVA, DVA,
 *     FCA and FBA; one backward pass carries the adjoints of all of them down
 *     to the mark, and the tape is rewound;
 *  4. per batch of 64 paths, the adjoints accumulated at the mark are
 *     propagated to the leaves: a batch's risks. Their dispersion over the
 *     batches is the Monte-Carlo error of each sensitivity.
 *
 * The same code runs on plain doubles (xva_values()), which is what the
 * sensitivities are checked against by finite differences, on the same
 * scenarios, and what the cost of the adjoint is measured against.
 *
 * What is differentiated is the path-by-path estimator, so what jumps is
 * smoothed or left out:
 *  - a swaption physically settled becomes a swap or nothing at its expiry.
 *    The indicator is replaced by a ramp over a narrow band of the swap's
 *    value (Savine's fuzzy logic), `exercise_smoothing` standard deviations
 *    wide; without it the sensitivities miss the paths that cross the
 *    exercise boundary;
 *  - a minimum transfer amount and a rounding make the collateral jump: they
 *    are differentiated as they are, which leaves their jumps out.
 *
 * Covered: trades with a closed form (swaps, European swaptions), under one
 * curve, with or without variation margin. Not covered: trades valued by
 * regression, initial margin, wrong-way risk, capital.
 */

namespace quantModeling
{

    /// The adjustments differentiated together, in this order.
    enum class XvaOutput : std::size_t
    {
        Cva, ///< bilateral: first to default
        Dva,
        Fca,
        Fba,
        CvaUnilateral ///< the bank assumed default-free: the regulatory CVA
    };
    inline constexpr std::size_t kXvaOutputs = 5;

    enum class XvaRiskFactor
    {
        ZeroRate,           ///< continuously compounded zero rate of a curve pillar
        MeanReversion,      ///< Hull-White a
        Sigma,              ///< Hull-White σ
        CounterpartyHazard, ///< hazard rate of one period of the counterparty's curve
        OwnHazard,
        CounterpartyLgd,
        OwnLgd,
        BorrowingSpread,
        LendingSpread
    };

    struct XvaRiskInputs
    {
        CreditCurve counterparty{0.0};
        CreditCurve own{0.0};
        Real lgd_counterparty = 0.6;
        Real lgd_own = 0.6;
        Real borrowing_spread = 0.0;
        Real lending_spread = 0.0;
        /// The CSA (variation margin), or none.
        std::optional<Csa> csa;
        MarginPeriodCashflows cashflows = MarginPeriodCashflows::Paid;
        /**
         * @brief Half-width of the ramp that replaces the exercise indicator
         *        of an option, in standard deviations of the underlying's
         *        value at the expiry. 0 keeps the indicator (the pricing
         *        engine's values, and sensitivities that miss the boundary).
         */
        Real exercise_smoothing = 0.05;
        /// The half-widths themselves, one per trade (0 for a trade that is
        /// not an option): when given they replace the ones the run would
        /// compute, so that a bumped run smooths exactly as the base run did.
        std::vector<Real> smoothing_widths;
    };

    struct XvaValues
    {
        /// Indexed by XvaOutput.
        std::array<Estimate, kXvaOutputs> values;
        /// The half-width used for each trade, in currency units.
        std::vector<Real> smoothing_widths;
        std::size_t paths = 0;
        std::size_t batches = 0;
        /// Wall time of the paths, set-up excluded.
        double seconds = 0.0;

        const Estimate &operator[](XvaOutput o) const { return values[static_cast<std::size_t>(o)]; }
    };

    struct XvaRisks : XvaValues
    {
        std::vector<XvaRiskFactor> factors;
        /// The pillar of a zero rate, the end of a hazard period (0 for a
        /// flat curve), 0 otherwise.
        std::vector<Time> tenors;
        std::vector<std::string> labels;
        /// The value of each factor.
        std::vector<Real> levels;
        /// d output / d factor and its Monte-Carlo error, at
        /// [output × factors.size() + factor].
        std::vector<Real> risks;
        std::vector<Real> errors;
        /// The risks of each batch, at [(batch × kXvaOutputs + output) ×
        /// factors.size() + factor]: what a linear map of the factors (to
        /// market quotes, say) is applied to, batch by batch, to carry the
        /// Monte-Carlo error through it.
        std::vector<Real> batch_risks;

        std::size_t index(XvaOutput o, std::size_t factor) const
        {
            return static_cast<std::size_t>(o) * factors.size() + factor;
        }
        Estimate risk(XvaOutput o, std::size_t factor) const
        {
            return {risks[index(o, factor)], errors[index(o, factor)]};
        }
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_XVA_RISKS_HPP
