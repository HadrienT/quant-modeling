#include "quantModeling/engines/mc/script_engine.hpp"

#include <algorithm>

#include "quantModeling/engines/mc/importance_sampling.hpp"
#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
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

        /// Sobol RQMC replicates, as the generic engine counts them.
        int rqmc_replicates(const PricingSettings &s)
        {
            return std::max(2, s.mc_rqmc_batches);
        }

        /**
         * One GPU run of a script: the generic engine's units, draws and
         * estimators (engines/mc/simulation_engine.hpp) -- Philox, stratified
         * or Sobol RQMC, with spot controls and an importance-sampling drift
         * when given -- so that it gives the CPU's numbers.
         */
        SimulationMCResult run_on_gpu(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                      const DeviceModel &dm, const std::vector<std::vector<Time>> &mats,
                                      const PricingSettings &settings, const std::vector<double> *theta)
        {
            const int requested = settings.mc_paths > 0 ? settings.mc_paths : 100000;
            const bool sobol = settings.mc_sampler == SamplerKind::Sobol;
            const bool stratified = settings.mc_sampler == SamplerKind::Stratified;
            gpu::ScriptGpuRequest req;
            req.program = &product.program();
            req.model = &dm;
            req.baseline = product.baseline();
            req.discount_mats = mats;
            req.antithetic = settings.mc_antithetic && !sobol; // the CPU's Sobol has no mirror
            req.seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
            req.devices = gpu::devices_for(settings.mc_gpus); // the same bits on one card or all (lot G4)
            const auto n_units = static_cast<uint64_t>(req.antithetic ? (requested + 1) / 2 : requested);
            req.n_units = n_units;
            if (theta)
                req.is_theta = *theta;

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

            std::optional<mc::SobolTables> tables;
            int B = 1;
            if (stratified)
            {
                req.stratified = true;
                req.replicates = B = rqmc_replicates(settings);
                req.n_units = std::max<uint64_t>(1, n_units / static_cast<uint64_t>(B));
            }
            if (sobol)
            {
                B = rqmc_replicates(settings);
                req.n_units = static_cast<uint64_t>(std::max(1, requested / B));
                tables = mc::sobol_tables(model.sim_dim(), req.seed, B, model.brownian_layout(),
                                          settings.mc_brownian_bridge);
                req.sobol = &*tables;
            }
            const gpu::ScriptGpuStats st = gpu::simulate_script(req);

            SimulationMCResult res;
            res.labels = product.payoff_labels();
            std::string note = "SimulationMCEngine on GPU (" +
                               gpu::devices_label(std::vector<int>(req.devices.begin(),
                                                                   req.devices.begin() + gpu::gpus_used(req))) +
                               ")";
            std::string cv_note = k > 0 ? " + spot control" : "";
            if (!stratified && !sobol)
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
                note += " + Philox";
            }
            else
            {
                // Independent replicates: the error is their spread.
                WelfordAccumulator reps;
                for (const auto &c : st.stratified_controlled)
                    reps.add(c.estimate(controls.mean, k));
                for (const auto &c : st.controlled)
                    reps.add(c.estimate(controls.mean, k).value);
                for (const auto &w : st.plain)
                    reps.add(w.mean);
                res.values = {reps.mean};
                res.std_errors = {reps.std_error()};
                res.n_paths = static_cast<long long>(req.n_units) * B * (req.antithetic ? 2 : 1);
                if (stratified)
                {
                    note += " + Philox + stratified W(T) (" + std::to_string(B) + " replicates of " +
                            std::to_string(req.n_units) + " strata)";
                    if (k > 0)
                        cv_note = " + spot control (within-stratum slope)";
                }
                else
                    note += " + Sobol RQMC (" + std::to_string(B) + " batches, dim=" +
                            std::to_string(model.sim_dim()) + ")" +
                            (tables->bridged() ? " + Brownian bridge" : "");
            }
            res.diagnostics = note + (req.antithetic ? " + antithetic" : "") + cv_note + cv_off;
            res.device = "gpu";
            res.gpus = gpu::gpus_used(req);
            return res;
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
        model.init(product.timeline(), product.defline());
        if (!model.describe_device(dm))
            why = "this model runs on the CPU only (the GPU simulates Black-Scholes with a flat rate, "
                  "local vol, Heston and SLV)";
        else if (settings.mc_sampler == SamplerKind::Sobol &&
                 model.sim_dim() > static_cast<std::size_t>(sobol_detail::kMaxDimension))
            why = "more draws per path than the " + std::to_string(sobol_detail::kMaxDimension) +
                  " Sobol dimensions";
        else
        {
            mats = discount_lists(product);
            why = gpu::script_gpu_unsupported(product.program(), dm, mats);
        }
        if (!why.empty())
        {
            if (explicit_gpu)
                throw InvalidInput("GPU: " + why);
            SimulationMCResult res = simulate<Real>(product, model, settings);
            res.diagnostics += " (on the CPU: " + why + ")";
            return res;
        }

        if (!settings.mc_importance_drift)
            return run_on_gpu(product, model, dm, mats, settings, nullptr);

        // Importance sampling as simulate() decides it: the drift searched on
        // the host, the pilot -- same seed and draws as the CPU's -- on the
        // device.
        const mc::ImportanceDrift drift = mc::find_importance_drift(product, model);
        if (drift.theta.empty())
        {
            SimulationMCResult res = run_on_gpu(product, model, dm, mats, settings, nullptr);
            res.diagnostics += " (no importance sampling: " + drift.why_none + ")";
            return res;
        }
        const PricingSettings pilot = detail::importance_pilot(settings);
        const Real plain_se = run_on_gpu(product, model, dm, mats, pilot, nullptr).std_error();
        const Real is_se = run_on_gpu(product, model, dm, mats, pilot, &drift.theta).std_error();
        if (!mc::importance_sampling_pays(plain_se, is_se))
        {
            SimulationMCResult res = run_on_gpu(product, model, dm, mats, settings, nullptr);
            res.diagnostics += " (no importance sampling: the drift did not lower the pilot's variance)";
            return res;
        }
        SimulationMCResult res = run_on_gpu(product, model, dm, mats, settings, &drift.theta);
        res.diagnostics += mc::importance_note(drift, plain_se, is_se);
        return res;
    }

    std::optional<AADSimulResults> simulate_script_aad_gpu(const ScriptedProduct<Real> &product,
                                                           ISimulationModel<Real> &model, std::size_t n_paths,
                                                           std::uint64_t seed, std::string &why,
                                                           SamplerKind sampler, int max_gpus)
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
        const bool sobol = sampler == SamplerKind::Sobol;
        if (sobol && model.sim_dim() > static_cast<std::size_t>(sobol_detail::kMaxDimension))
        {
            why = "more draws per path than the Sobol dimensions";
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
        req.devices = gpu::devices_for(max_gpus);
        // Sobol: simulate_aad's replicates (engines/mc/simulation_engine_aad.hpp).
        std::optional<mc::SobolTables> tables;
        if (sobol)
        {
            req.n_units = static_cast<uint64_t>(std::max<std::size_t>(1, n_paths / kAadRqmcReplicates));
            tables = mc::sobol_tables(model.sim_dim(), seed, kAadRqmcReplicates, model.brownian_layout(), true);
            req.sobol = &*tables;
        }
        const std::string draws = sobol ? " + Sobol RQMC (" + std::to_string(kAadRqmcReplicates) + " batches)" +
                                              (tables->bridged() ? " + Brownian bridge" : "")
                                        : " + Philox";

        auto run = [&]() -> AADSimulResults
        {
            AADSimulResults res;
            res.risk_labels = model.parameter_labels();
            res.n_paths = static_cast<long long>(req.n_units) * (sobol ? kAadRqmcReplicates : 1);
            res.risks.assign(res.risk_labels.size(), 0.0);
            res.risk_std_errors.assign(res.risk_labels.size(), 0.0);
            res.gpus = gpu::gpus_used(req);
            const std::string card = gpu::devices_label(
                std::vector<int>(req.devices.begin(), req.devices.begin() + res.gpus));
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
                                  (sobol ? " risks)" : " risks, batches of 512)") + draws;
            }
            else
            {
                const auto reps = gpu::simulate_script_duals(req);
                if (!sobol)
                {
                    const std::vector<WelfordAccumulator> &w = reps.front();
                    res.price = w[0].mean;
                    res.price_std_error = w[0].std_error();
                    for (std::size_t i = 0; i + 1 < w.size() && i < res.risks.size(); ++i)
                    {
                        res.risks[i] = w[i + 1].mean;
                        res.risk_std_errors[i] = w[i + 1].std_error();
                    }
                }
                else
                {
                    // Replicate means are i.i.d.: their spread is the error.
                    std::vector<WelfordAccumulator> acc(reps.front().size());
                    for (const auto &w : reps)
                        for (std::size_t i = 0; i < w.size(); ++i)
                            acc[i].add(w[i].mean);
                    res.price = acc[0].mean;
                    res.price_std_error = acc[0].std_error();
                    for (std::size_t i = 0; i + 1 < acc.size() && i < res.risks.size(); ++i)
                    {
                        res.risks[i] = acc[i + 1].mean;
                        res.risk_std_errors[i] = acc[i + 1].std_error();
                    }
                }
                res.diagnostics =
                    "forward-mode duals on GPU (" + card + ", " + std::to_string(dirs) + " directions)" + draws;
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
