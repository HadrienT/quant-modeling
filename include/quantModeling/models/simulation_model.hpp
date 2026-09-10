#ifndef QM_MODELS_SIMULATION_MODEL_HPP
#define QM_MODELS_SIMULATION_MODEL_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <memory>
#include <span>
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
     * and, later, an AAD tape type. Only T = Real is instantiated today.
     */
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
    };

} // namespace quantModeling

#endif // QM_MODELS_SIMULATION_MODEL_HPP
