#include "quantModeling/engines/mc/script_engine.hpp"

#include <algorithm>

#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/spot_controls.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/models/device_model.hpp"

namespace quantModeling
{

    namespace
    {
        std::vector<std::vector<Time>> discount_lists(const ScriptedProduct<Real> &product)
        {
            std::vector<std::vector<Time>> mats;
            for (const SampleDef &def : product.defline())
                mats.push_back(def.discount_mats);
            return mats;
        }
    } // namespace

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
        if (settings.mc_sampler == SamplerKind::Sobol)
            why = "scripts run pseudo-random or stratified on the GPU (Sobol stays on the CPU)";
        else
        {
            model.init(product.timeline(), product.defline());
            if (!model.describe_device(dm))
                why = "this model runs on the CPU only (the GPU simulates Black-Scholes with a flat rate, "
                      "local vol, Heston and SLV)";
            else
            {
                mats = discount_lists(product);
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

        // The CPU generic engine's units, draws and estimators, on the device.
        const int requested = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.model = &dm;
        req.baseline = product.baseline();
        req.discount_mats = std::move(mats);
        req.antithetic = settings.mc_antithetic;
        req.seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
        const auto n_units = static_cast<uint64_t>(req.antithetic ? (requested + 1) / 2 : requested);
        req.n_units = n_units;

        mc::SpotControlSet controls;
        std::string cv_off;
        if (settings.mc_spot_control)
        {
            controls = mc::choose_spot_controls(product.timeline(), model);
            req.controls = controls.sel;
            if (controls.sel.n == 0)
                cv_off = " (no spot control: " + controls.why_none + ")";
        }
        const int k = controls.sel.n;
        const bool stratified = settings.mc_sampler == SamplerKind::Stratified;
        if (stratified)
        {
            req.stratified = true;
            req.replicates = std::max(2, settings.mc_rqmc_batches);
            req.n_units = std::max<uint64_t>(1, n_units / static_cast<uint64_t>(req.replicates));
        }
        const gpu::ScriptGpuStats st = gpu::simulate_script(req);

        SimulationMCResult res;
        res.labels = product.payoff_labels();
        std::string note = "SimulationMCEngine on GPU (" + gpu::device_name(req.device) + ") + Philox";
        std::string cv_note = k > 0 ? " + spot control" : "";
        if (!stratified)
        {
            if (k > 0)
            {
                const auto e = st.controlled.front().estimate(controls.mean, k);
                res.values = {e.value};
                res.std_errors = {e.std_error};
                cv_note = " + spot control (" + std::to_string(e.used) + " of " + std::to_string(k) + " kept" +
                          detail::variance_ratio_note(e) + ")";
            }
            else
            {
                res.values = {st.plain.front().mean};
                res.std_errors = {st.plain.front().std_error()};
            }
            res.n_paths = static_cast<long long>(n_units) * (req.antithetic ? 2 : 1);
        }
        else
        {
            WelfordAccumulator reps;
            for (const auto &c : st.stratified_controlled)
                reps.add(c.estimate(controls.mean, k));
            for (const auto &w : st.plain)
                reps.add(w.mean);
            if (k > 0)
                cv_note = " + spot control (within-stratum slope)";
            res.values = {reps.mean};
            res.std_errors = {reps.std_error()};
            res.n_paths = static_cast<long long>(req.n_units) * req.replicates * (req.antithetic ? 2 : 1);
            note += " + stratified W(T) (" + std::to_string(req.replicates) + " replicates of " +
                    std::to_string(req.n_units) + " strata)";
        }
        res.diagnostics = note + (req.antithetic ? " + antithetic" : "") + cv_note + cv_off;
        res.device = "gpu";
        return res;
    }

    std::optional<AADSimulResults> simulate_script_aad_gpu(const ScriptedProduct<Real> &product,
                                                           ISimulationModel<Real> &model, std::size_t n_paths,
                                                           std::uint64_t seed, std::string &why)
    {
        if (gpu::device_count() == 0)
        {
            why = "this server has no usable CUDA device";
            return std::nullopt;
        }
        model.init(product.timeline(), product.defline());
        DeviceModel dm;
        if (!model.describe_device(dm))
        {
            why = "this model's risks run on the CPU only (the GPU differentiates Black-Scholes, Heston and "
                  "local vol)";
            return std::nullopt;
        }
        const auto mats = discount_lists(product);
        why = gpu::script_gpu_unsupported(product.program(), dm, mats);
        if (!why.empty())
            return std::nullopt;
        const bool adjoint = dm.kind == DeviceModel::Kind::LocalVol;
        const int dirs = mc::dual_directions(dm.kind, dm.n_assets);
        if (dm.kind == DeviceModel::Kind::Heston && dm.jump_k != 0.0)
        {
            why = "jump parameters are set: the GPU does not differentiate the jump compensator";
            return std::nullopt;
        }
        if (!adjoint && (dirs <= 0 || dirs > 8))
        {
            why = dm.kind == DeviceModel::Kind::SLV
                      ? "SLV risks run on the CPU (the GPU differentiates Black-Scholes, Heston and local vol)"
                      : "more than two assets: the GPU's forward-mode risks carry at most 8 parameters";
            return std::nullopt;
        }

        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.model = &dm;
        req.baseline = product.baseline();
        req.discount_mats = mats;
        req.antithetic = false; // the CPU adjoint's paths, one per unit
        req.n_units = static_cast<uint64_t>(n_paths);
        req.seed = seed;

        auto run = [&]() -> AADSimulResults
        {
            AADSimulResults res;
            res.risk_labels = model.parameter_labels();
            res.n_paths = static_cast<long long>(n_paths);
            res.risks.assign(res.risk_labels.size(), 0.0);
            res.risk_std_errors.assign(res.risk_labels.size(), 0.0);
            const std::string card = gpu::device_name(req.device);
            if (adjoint)
            {
                const gpu::ScriptAdjointGpuResult r = gpu::simulate_script_adjoint(req);
                res.price = r.price.mean;
                res.price_std_error = r.price.std_error();
                for (std::size_t i = 0; i < res.risks.size() && i < r.risks.size(); ++i)
                {
                    res.risks[i] = r.risks[i];
                    res.risk_std_errors[i] = r.risk_std_errors[i];
                }
                res.diagnostics = "per-path adjoint on GPU (" + card + ", " + std::to_string(r.risks.size()) +
                                  " risks, batches of 512) + Philox";
            }
            else
            {
                const std::vector<WelfordAccumulator> w = gpu::simulate_script_duals(req);
                res.price = w[0].mean;
                res.price_std_error = w[0].std_error();
                for (std::size_t i = 0; i + 1 < w.size() && i < res.risks.size(); ++i)
                {
                    res.risks[i] = w[i + 1].mean;
                    res.risk_std_errors[i] = w[i + 1].std_error();
                }
                res.diagnostics = "forward-mode duals on GPU (" + card + ", " + std::to_string(dirs) +
                                  " directions) + Philox";
            }
            return res;
        };
        // A card short of memory (the LLM shares it) is a reason to stay on
        // the CPU, not an error of the request.
        try
        {
            return run();
        }
        catch (const gpu::GpuUnavailable &e)
        {
            why = e.what();
            return std::nullopt;
        }
    }

} // namespace quantModeling
