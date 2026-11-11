#ifndef QM_INSTRUMENTS_SIMULATABLE_HPP
#define QM_INSTRUMENTS_SIMULATABLE_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"

#include <memory>
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

        /// Per-path working state of payoffs(), one per thread.
        struct PayoffState
        {
            virtual ~PayoffState() = default;
        };

        /**
         * @brief Several threads at once (blueprint/wp/26-performance.md,
         *        P1e): a fresh working state for one thread, or null when
         *        payoffs() cannot run concurrently (the default) -- the engine
         *        then stays on one thread.
         */
        virtual std::unique_ptr<PayoffState> make_payoff_state() const { return nullptr; }

        /// payoffs() with the calling thread's own state (from
        /// make_payoff_state()): same values, safe to call concurrently.
        virtual void payoffs_with_state(const Scenario<T> &path, std::vector<T> &out, PayoffState & /*state*/) const
        {
            payoffs(path, out);
        }

        /**
         * @brief Several paths per call (blueprint/wp/26-performance.md, P1a):
         *        up to batch_width() paths whose single payoff goes to
         *        out[i], with the values payoffs() gives path by path, bit
         *        for bit. 0 (the default): not available; the engine calls
         *        payoffs() per path.
         */
        virtual int batch_width() const { return 0; }

        /// A fresh batch state for one thread (batch_width() > 0 only).
        virtual std::unique_ptr<PayoffState> make_batch_state() const { return nullptr; }

        virtual void payoffs_batch(const Scenario<T> *const * /*paths*/, int /*count*/, T * /*out*/,
                                   PayoffState & /*state*/) const
        {
        }
    };

} // namespace quantModeling

#endif // QM_INSTRUMENTS_SIMULATABLE_HPP
