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
     * cannot (a Sobol sampler, a model or a script the kernel does not
     * support); Auto takes the GPU when a device is present and the request
     * fits, the CPU otherwise, and says so in the diagnostics. A GPU run is
     * Philox, as is the CPU run it is checked against (mc_rng = Philox).
     * The generic variance reduction (mc_spot_control, SamplerKind::
     * Stratified) runs on either side, with the same estimators.
     */
    SimulationMCResult simulate_script(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                       const PricingSettings &settings);

    /**
     * @brief A script's model risks on the GPU (blueprint/wp/19-gpu.md §6,
     *        lot G3): forward-mode duals under Black-Scholes (one or two
     *        assets) and Heston, the per-path adjoint under local vol (every
     *        sigma_loc point: the superbucket's input).
     *
     * Same labels, same Philox paths as simulate_aad(..., RngKind::Philox),
     * its oracle. nullopt, with `why` in words a user can read, when the GPU
     * cannot take the request (no device, SLV, jumps, a script too large).
     */
    std::optional<AADSimulResults> simulate_script_aad_gpu(const ScriptedProduct<Real> &product,
                                                           ISimulationModel<Real> &model, std::size_t n_paths,
                                                           std::uint64_t seed, std::string &why);

} // namespace quantModeling

#endif // QM_ENGINES_MC_SCRIPT_ENGINE_HPP
