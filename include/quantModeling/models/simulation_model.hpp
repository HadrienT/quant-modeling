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

    struct DeviceModel; // models/device_model.hpp
    namespace mc
    {
        enum class VarianceScheme : int; // models/equity/path_steps.hpp
    }

    /// Throws when the defline asks for continuous barrier monitoring
    /// (survivalAbove / survivalBelow, WP 26 P2): a model that does not
    /// compute the bridge survival probabilities calls it from init(), so
    /// that a script never reads survivals nobody filled.
    inline void refuse_step_monitors(const std::vector<SampleDef> &defline, const char *model)
    {
        for (const SampleDef &def : defline)
            if (!def.monitors.empty())
                throw InvalidInput(std::string(model) +
                                   " does not compute quantities over its steps: survivalAbove() / survivalBelow() "
                                   "need Black-Scholes or local vol, knockOutAbove() / knockOutBelow() "
                                   "Black-Scholes, local vol, Heston or SLV, "
                                   "lastStepCall() / lastStepPut() / lastStepDigital() Black-Scholes, local vol, "
                                   "Heston or SLV");
    }

    /// Throws when the defline asks for a bridge survival (survivalAbove /
    /// survivalBelow): for a model that computes the last-step expectations
    /// but not the survivals (Heston, SLV).
    inline void refuse_bridge_survivals(const std::vector<SampleDef> &defline, const char *model)
    {
        for (const SampleDef &def : defline)
            for (const StepMonitor &b : def.monitors)
                if (b.kind == StepQuantity::Survival)
                    throw InvalidInput(std::string(model) +
                                       " does not monitor barriers between dates: survivalAbove() / survivalBelow() "
                                       "need Black-Scholes or local vol");
    }

    /// Throws when the defline asks for a conditioned knock-out
    /// (knockOutAbove / knockOutBelow, issue #369): for a model whose step
    /// into a date is not lognormal given the path (rough Bergomi, jumps,
    /// Hull-White's equity).
    inline void refuse_knock_outs(const std::vector<SampleDef> &defline, const char *model)
    {
        for (const SampleDef &def : defline)
            for (const StepMonitor &b : def.monitors)
                if (b.kind == StepQuantity::KnockOut)
                    throw InvalidInput(std::string(model) +
                                       " does not condition its steps on a knock-out: knockOutAbove() / "
                                       "knockOutBelow() need Black-Scholes, local vol, Heston or SLV");
    }

    /// Checks the barriers a model that monitors them is asked for: each one
    /// on an asset it simulates.
    inline void check_barrier_assets(const std::vector<SampleDef> &defline, std::size_t n_assets, const char *model)
    {
        for (const SampleDef &def : defline)
            for (const StepMonitor &b : def.monitors)
                if (b.asset >= n_assets)
                    throw InvalidInput(std::string(model) + ": a barrier on asset " + std::to_string(b.asset) +
                                       ", but the model simulates " + std::to_string(n_assets) + " asset(s)");
    }

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

        /// Whether generate_path() fills Sample::monitored for the barriers
        /// of the defline (WP 26 P2). A model that returns false refuses a
        /// defline with barriers in init() (refuse_step_monitors).
        virtual bool monitors_barriers() const { return false; }

        /**
         * @brief Multilevel Monte Carlo (blueprint/wp/26-performance.md §8.3):
         *        split every step of the model's own grid into `r` equal
         *        steps. Called before init(). A model that discretises in
         *        time (log-Euler local vol) accepts any r >= 1 and returns
         *        true; a model with no time-step bias, or not yet able to,
         *        keeps the default and accepts only r = 1. The coupling of
         *        two levels is the engine's (engines/mc/mlmc.hpp), through
         *        brownian_layout(): the model only steps on its grid.
         */
        virtual bool set_refinement(int r) { return r == 1; }

        /**
         * @brief Multilevel Monte Carlo: condition the coarse path's step
         *        quantities on the fine path (Giles & Szpruch 2012, §3.3-3.5;
         *        blueprint/wp/26-performance.md §26).
         *
         * For each drawing step k of this (coarse) model, `half[k]` is the
         * part of its normalised Brownian draw z_k (factor 0) that the fine
         * level's first sub-step carries, and `share[k]` that sub-step's
         * share of the step: z_k = half[k] + sqrt(1 - share[k]) z', z' an
         * independent N(0, 1). A model that returns true computes its bridge
         * survivals through the midpoint the fine path fixes, and its
         * lastStep* expectations given half[k], on the next generate_path()
         * calls -- the conditional expectations of the same estimators, so
         * every level keeps its mean (tower property) and the corrections
         * lose the O(h^1/2) terms of conditioning the fine and the coarse
         * path at different times. Empty spans: unconditional again. The
         * spans must outlive the calls. Called after init(); the default
         * declines.
         */
        virtual bool condition_on_fine_half_steps(std::span<const double> /*half*/,
                                                  std::span<const double> /*share*/)
        {
            return false;
        }

        /// Whether the model simulates the product's dates exactly, with no
        /// time step between them (Black-Scholes): its discretisation bias
        /// is then 0 (WP 26 §9.4). False by default -- a model that steps on
        /// a grid of its own, even one it does not expose, must not claim
        /// it.
        virtual bool exact_between_dates() const { return false; }

        /// Valid after init(). Models without Brownian increments to reorder
        /// (or not yet described) keep the default: no bridge.
        virtual BrownianLayout brownian_layout() const { return {}; }

        /// Valid after init(): describe this model as plain data for the GPU
        /// script engine (blueprint/wp/19-gpu.md §4). False when the device
        /// cannot simulate it (another model, a term structure of rates, a
        /// jump, an AAD number type): the caller then stays on the CPU.
        virtual bool describe_device(DeviceModel &) const { return false; }

        /// Heston and SLV: step the variance of a price run (T = Real) by
        /// `scheme` (#317). False when the model has no such choice. A model
        /// differentiated (T = aad::Number) always steps by Euler.
        virtual bool set_variance_scheme(mc::VarianceScheme /*scheme*/) { return false; }

        /// E[S_asset(t) / N(t)], the deflated spot's mean, when the model's
        /// discretisation keeps it a martingale -- what the generic control
        /// variates need (blueprint/wp/19-gpu.md §2.5). False when unknown.
        virtual bool deflated_spot_mean(std::size_t /*asset*/, Time /*t*/, Real & /*mean*/) const
        {
            return false;
        }

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
