#ifndef QM_ENGINES_MC_SCRIPT_ENGINE_HPP
#define QM_ENGINES_MC_SCRIPT_ENGINE_HPP

#include "quantModeling/engines/mc/simulation_engine.hpp"
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
     */
    SimulationMCResult simulate_script(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                       const PricingSettings &settings);

} // namespace quantModeling

#endif // QM_ENGINES_MC_SCRIPT_ENGINE_HPP
