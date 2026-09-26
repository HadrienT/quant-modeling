#ifndef QM_MODELS_SIMULATION_MODEL_HPP
#define QM_MODELS_SIMULATION_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace quantModeling
{

    /**
     * @brief A model that can simulate scenarios for the timeline architecture.
     *
     * Lifecycle:
     *  1. init(product_timeline, defline) — once. The model merges the product
     *     timeline with its own discretisation steps into sim_timeline() and
     *     precomputes per-step coefficients.
     *  2. generate_path(gaussians, path) — per Monte-Carlo path. `gaussians`
     *     has sim_dim() entries; `path` is pre-sized to defline.size() samples
     *     (see allocate_scenario). The model writes spots / discounts /
     *     forwards / numeraire at each event date.
     *  3. clone() — one independent copy per worker thread.
     *
     * Templated on the number type T so the same class serves double pricing
     * and an AAD tape type (blueprint/wp/17-aad.md §6.2): with T = aad::Number,
     * parameters() exposes non-owning pointers to whichever of the model's own
     * members are differentiable parameters (a spot, a flat rate, a local-vol
     * grid point...), so a Monte-Carlo engine can put them on the tape and
     * read their adjoints back as model risks, without knowing anything about
     * the specific model.
     */
    /**
     * @brief Which of a model's per-path gaussians are Brownian increments,
     *        and on which time grid (blueprint/wp/19-gpu.md §2.4).
     *
     * The gaussians come in `times.size()` consecutive blocks of `stride`,
     * one block per drawing step, in time order; the first `factors` entries
     * of each block are the step's independent Brownian increments, scaled
     * to N(0,1) (correlation, if any, is applied by the model afterwards);
     * the remaining `stride - factors` are other draws (jump counts and
     * sizes). An empty layout (factors == 0) means "no Brownian structure to
     * exploit": the engine then feeds the draws in plain time order.
     */
    struct BrownianLayout
    {
        std::vector<Time> times; ///< end of each drawing step, strictly increasing, > 0
        std::size_t factors = 0;
        std::size_t stride = 0;

        bool empty() const { return factors == 0 || times.empty(); }
        /// The layout describes exactly `dim` gaussians.
        bool covers(std::size_t dim) const
        {
            return !empty() && stride >= factors && times.size() * stride == dim;
        }
    };

    template <class T = Real>
    struct ISimulationModel
    {
        virtual ~ISimulationModel() = default;

        virtual std::size_t n_underlyings() const = 0;

        virtual void init(const TimeLine &product_timeline,
                          const std::vector<SampleDef> &defline) = 0;

        virtual const TimeLine &sim_timeline() const = 0;
        virtual std::size_t sim_dim() const = 0;

        virtual void generate_path(std::span<const double> gaussians,
                                   Scenario<T> &path) const = 0;

        virtual std::unique_ptr<ISimulationModel<T>> clone() const = 0;

        /// Valid after init(). Models without Brownian increments to reorder
        /// (or not yet described) keep the default: no bridge.
        virtual BrownianLayout brownian_layout() const { return {}; }

        /// Non-owning pointers to the model's own differentiable parameters.
        /// A book trap to reproduce exactly: these pointers are invalidated
        /// by copying the model (the copy's own members live at different
        /// addresses) -- every model recomputes them in its constructor
        /// *and* in a hand-written copy constructor via set_param_pointers(),
        /// or clone() silently hands back a model whose parameters still
        /// point into the original, and a parallel run computes wrong
        /// sensitivities without ever crashing.
        virtual const std::vector<T *> &parameters() const = 0;
        virtual const std::vector<std::string> &parameter_labels() const = 0;
        std::size_t num_params() const { return parameters().size(); }

        /// Registers every parameter as a fresh tape leaf. A no-op for
        /// T = Real; called once per simulate_aad() run, before init(), so
        /// the model's precomputations are themselves recorded in terms of
        /// these leaves (blueprint §7.1).
        void put_parameters_on_tape() const
        {
            if constexpr (std::is_same_v<T, aad::Number>)
                for (T *p : parameters())
                    p->put_on_tape();
        }
    };

} // namespace quantModeling

#endif // QM_MODELS_SIMULATION_MODEL_HPP
