#ifndef QM_INSTRUMENTS_SIMULATABLE_HPP
#define QM_INSTRUMENTS_SIMULATABLE_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"

#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief A product expressed for the timeline simulation architecture.
     *
     * The product publishes the dates at which it looks at the market
     * (timeline(), as year-fractions from the valuation date — resolved from
     * Date via a ValuationContext at construction) and what it needs sampled
     * there (defline()). Given one simulated Scenario it returns its
     * numeraire-deflated payoff(s), one per payoff_labels() entry.
     *
     * One SimulationMCEngine prices every ISimulatableProduct; there is no
     * per-product engine and no visitor dispatch here.
     */
    template <class T = Real>
    struct ISimulatableProduct
    {
        virtual ~ISimulatableProduct() = default;

        virtual const TimeLine &timeline() const = 0;
        virtual const std::vector<SampleDef> &defline() const = 0;
        virtual const std::vector<std::string> &payoff_labels() const = 0;
        virtual std::size_t n_underlyings() const = 0;

        /// Deflated payoff(s) for one path. `out` is resized to
        /// payoff_labels().size(). Values are already divided by the numeraire
        /// (discounted to the valuation date).
        virtual void payoffs(const Scenario<T> &path,
                             std::vector<T> &out) const = 0;
    };

} // namespace quantModeling

#endif // QM_INSTRUMENTS_SIMULATABLE_HPP
