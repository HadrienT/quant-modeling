#ifndef QM_ENGINES_MC_SCRIPT_ENGINE_HPP
#define QM_ENGINES_MC_SCRIPT_ENGINE_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/pricers/context.hpp"

namespace quantModeling
{

    /**
     * @brief Price a script on the CPU or the GPU (blueprint/wp/19-gpu.md §8,
     *        lot G2).
     *
     * settings.mc_device picks: Cpu runs the generic engine (simulate());
     * Gpu runs the compiled script on the device, or throws saying why it
     * cannot (a model or a script the kernel does not support); Auto takes the GPU when a device is present and the request
     * fits, the CPU otherwise, and says so in the diagnostics. A GPU run is
     * Philox or Sobol RQMC (with the Brownian bridge), as is the CPU run it
     * is checked against (mc_rng = Philox). The generic variance reduction
     * (mc_spot_control, SamplerKind::Stratified, mc_importance_drift) runs
     * on either side, with the same estimators.
     */
    SimulationMCResult simulate_script(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                       const PricingSettings &settings, ThreadPool *pool = nullptr);

    /**
     * @brief A script's model risks on the GPU (blueprint/wp/19-gpu.md §6,
     *        lot G3): forward-mode duals under Black-Scholes (one or two
     *        assets) and Heston, the per-path adjoint under local vol (every
     *        sigma_loc point: the superbucket's input).
     *
     * Same labels, same paths as simulate_aad(..., RngKind::Philox, sampler),
     * its oracle -- Philox, or Sobol RQMC with the Brownian bridge
     * (kAadRqmcReplicates replicates). nullopt, with `why` in words a user
     * can read, when the GPU cannot take the request (no device, SLV, jumps,
     * a script too large). max_gpus: as PricingSettings::mc_gpus.
     */
    std::optional<AADSimulResults> simulate_script_aad_gpu(const ScriptedProduct<Real> &product,
                                                           ISimulationModel<Real> &model, std::size_t n_paths,
                                                           std::uint64_t seed, std::string &why,
                                                           SamplerKind sampler = SamplerKind::PseudoRandom,
                                                           int max_gpus = 0);

    /**
     * @brief A script's price and model risks by multilevel QMC on the GPU
     *        (blueprint/wp/26-performance.md §10.2): simulate_script's
     *        multilevel levels, each launch the per-path adjoint instead of
     *        the price kernel. A risk is the sum over the levels of the
     *        fine-minus-coarse risks (Burgos & Giles 2012), its error the
     *        replicates' spread; the points per level follow the price's
     *        target (eps, or rel_eps of the price floored at eps), as in
     *        MlqmcSettings.
     *
     * Local vol (the adjoint's model). nullopt, with `why`, when the GPU
     * cannot take the request or the script does not suit multilevel.
     */
    std::optional<AADSimulResults> simulate_script_aad_mlqmc_gpu(const ScriptedProduct<Real> &product,
                                                                 const ISimulationModel<Real> &model, double eps,
                                                                 double rel_eps, std::uint64_t seed,
                                                                 std::string &why, int max_gpus = 0);

    /**
     * @brief A script's model risks on the CPU without the tape
     *        (blueprint/wp/27-ir-ad.md §7, lot L7): the adjoint blocks built
     *        from the script's IR, run by the IR's evaluator -- a checkpoint
     *        of the state per step and per date instead of a node per
     *        operation.
     *
     * simulate_aad(..., RngKind::Philox)'s paths, labels and estimates
     * (risks averaged over batches of 64 paths, their error the batches'
     * spread), its oracle. Pseudo-random Philox paths. nullopt, with `why`,
     * for what the IR does not cover (a model without a device description,
     * early exercise, survivalAbove / Below, jumps).
     */
    std::optional<AADSimulResults> simulate_script_aad_ir(const ScriptedProduct<Real> &product,
                                                          ISimulationModel<Real> &model, std::size_t n_paths,
                                                          std::uint64_t seed, std::string &why);

    /// A script's second-order risks (WP 27 §9, §14.10).
    struct ScriptSecondOrderResults
    {
        std::vector<std::string> labels; ///< the model's risks (its parameter_labels)
        /// d2 price / d spot d theta_i, for every risk i: gamma, then the
        /// spot's cross-derivatives (vanna against a vol parameter or a
        /// grid point).
        std::vector<Real> spot_row, spot_row_std_errors;
        Real gamma = 0.0, gamma_std_error = 0.0;
        /// d delta / d vol and d vega / d vol: against the vol parameter
        /// (Black-Scholes) or a parallel shift of the local-vol grid; unset
        /// for a model without one vol (Heston, SLV).
        std::optional<Real> vanna, vanna_std_error, volga, volga_std_error;
        Real spot_bump = 0.0, vol_bump = 0.0;
        std::string method;      ///< how they were computed, in words a user can read
        std::string diagnostics; ///< the first-order runs' diagnostics
    };

    /**
     * @brief Second-order risks by the bump on the adjoint (WP 27 §8, rung
     *        4): central differences of the GPU's first-order risks, at the
     *        spot bumped by +-rel_spot_bump (gamma and the spot's row) and the
     *        vol by +-vol_bump (volga; a parallel shift of a local-vol grid).
     *
     * Every run on the same Sobol RQMC tables: replicate r of the run up and
     * replicate r of the run down share their points, so each replicate
     * gives one difference and the replicates' spread is the error (paired,
     * not two independent errors added). Biased in h^2, robust: it needs no
     * regularity of the payoff, and it is what the local-vol grid needs --
     * the grid is linear by pieces in the spot, so a pathwise second
     * derivative misses its kinks (§14.10). nullopt, with `why`, when the
     * GPU cannot take the first-order runs, or when the payoff jumps (a crisp
     * comparison of the spot, not fuzzy): its pathwise delta is 0 on every
     * path, and so would its bump be.
     */
    std::optional<ScriptSecondOrderResults> simulate_script_second_order_gpu(
        const ScriptedProduct<Real> &product, const ISimulationModel<Real> &model, std::size_t n_paths,
        std::uint64_t seed, std::string &why, double rel_spot_bump = 1e-2, double vol_bump = 2.5e-3,
        int max_gpus = 0);

} // namespace quantModeling

#endif // QM_ENGINES_MC_SCRIPT_ENGINE_HPP
