#include "quantModeling/engines/mc/script_engine.hpp"

#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/models/device_model.hpp"

namespace quantModeling
{

    SimulationMCResult simulate_script(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                       const PricingSettings &settings)
    {
        const bool explicit_gpu = settings.mc_device == ComputeDevice::Gpu;
        const bool want_gpu = explicit_gpu || (settings.mc_device == ComputeDevice::Auto && gpu::device_count() > 0);
        if (!want_gpu)
            return simulate<Real>(product, model, settings);

        // Why the GPU cannot take this request, if it cannot.
        std::string why;
        DeviceModel dm;
        std::vector<std::vector<Time>> mats;
        if (settings.mc_sampler != SamplerKind::PseudoRandom)
            why = "scripts run pseudo-random only on the GPU (Sobol stays on the CPU)";
        else
        {
            model.init(product.timeline(), product.defline());
            if (!model.describe_device(dm))
                why = "this model runs on the CPU only (the GPU simulates Black-Scholes with a flat rate, "
                      "local vol, Heston and SLV)";
            else
            {
                for (const SampleDef &def : product.defline())
                    mats.push_back(def.discount_mats);
                why = gpu::script_gpu_unsupported(product.program(), dm, mats);
            }
        }
        if (!why.empty())
        {
            if (explicit_gpu)
                throw InvalidInput("GPU: " + why);
            SimulationMCResult res = simulate<Real>(product, model, settings);
            res.diagnostics += " (on the CPU: " + why + ")";
            return res;
        }

        const int requested = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.model = &dm;
        req.baseline = product.baseline();
        req.discount_mats = std::move(mats);
        req.antithetic = settings.mc_antithetic;
        req.n_units = static_cast<uint64_t>(req.antithetic ? (requested + 1) / 2 : requested);
        req.seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
        const WelfordAccumulator w = gpu::simulate_script(req);

        SimulationMCResult res;
        res.labels = product.payoff_labels();
        res.values = {w.mean};
        res.std_errors = {w.std_error()};
        res.n_paths = static_cast<long long>(req.n_units) * (req.antithetic ? 2 : 1);
        res.diagnostics = "SimulationMCEngine on GPU (" + gpu::device_name(req.device) + ") + Philox" +
                          (req.antithetic ? " + antithetic" : "");
        res.device = "gpu";
        return res;
    }

} // namespace quantModeling
