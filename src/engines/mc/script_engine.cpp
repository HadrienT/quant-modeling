#include "quantModeling/engines/mc/script_engine.hpp"

#include <algorithm>
#include <sstream>

#include "quantModeling/engines/mc/importance_sampling.hpp"
#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/engines/mc/mlqmc.hpp"
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
            req.ir = &product.ir();
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
            // The script's analytic control (WP 26 P2), as on the CPU.
            {
                Real mean = 0.0;
                if (settings.mc_control_variate && product.analytic_control(mean) &&
                    controls.sel.n < mc::SpotControls::kMax)
                {
                    const int c = controls.sel.n++;
                    controls.sel.event[c] = -1;
                    controls.sel.asset[c] = -static_cast<int>(product.control_variable() + 1);
                    controls.mean[c] = mean;
                    req.controls = controls.sel;
                    cv_off.clear();
                }
            }
            const int k = controls.sel.n;
            bool analytic = false;
            for (int c = 0; c < k; ++c)
                analytic = analytic || controls.sel.asset[c] < 0;
            const std::string cv_label =
                analytic ? (k > 1 ? " + analytic and spot controls" : " + analytic control") : " + spot control";

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
                req.n_units = static_cast<uint64_t>(
                    sobol_points_per_replicate(static_cast<std::size_t>(requested), static_cast<std::size_t>(B)));
                tables = mc::sobol_tables(model.sim_dim(), req.seed, B, model.brownian_layout(),
                                          settings.mc_brownian_bridge, settings.mc_sobol_owen);
                req.sobol = &*tables;
            }
            const gpu::ScriptGpuStats st = gpu::simulate_script(req);

            SimulationMCResult res;
            res.labels = product.payoff_labels();
            std::string note = "SimulationMCEngine on GPU (" +
                               gpu::devices_label(std::vector<int>(req.devices.begin(),
                                                                   req.devices.begin() + gpu::gpus_used(req))) +
                               ")";
            std::string cv_note = k > 0 ? cv_label : "";
            if (!stratified && !sobol)
            {
                if (k > 0)
                {
                    const auto e = st.controlled.front().estimate(controls.mean, k);
                    res.values = {e.value};
                    res.std_errors = {e.std_error};
                    cv_note = cv_label + " (" + std::to_string(e.used) + " of " + std::to_string(k) + " kept" +
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
                // Kept in replicate order: two runs with the same seed pair
                // up replicate by replicate (SimulationMCResult::replicates).
                WelfordAccumulator reps;
                for (const auto &c : st.stratified_controlled)
                    res.replicates.push_back(c.estimate(controls.mean, k));
                for (const auto &c : st.controlled)
                    res.replicates.push_back(c.estimate(controls.mean, k).value);
                for (const auto &w : st.plain)
                    res.replicates.push_back(w.mean);
                for (const Real x : res.replicates)
                    reps.add(x);
                res.values = {reps.mean};
                res.std_errors = {reps.std_error()};
                res.n_paths = static_cast<long long>(req.n_units) * B * (req.antithetic ? 2 : 1);
                if (stratified)
                {
                    note += " + Philox + stratified W(T) (" + std::to_string(B) + " replicates of " +
                            std::to_string(req.n_units) + " strata)";
                    if (k > 0)
                        cv_note = cv_label + " (within-stratum slope)";
                }
                else
                    note += " + Sobol RQMC (" + std::to_string(B) + " batches of " + std::to_string(req.n_units) +
                            " points, dim=" +
                            std::to_string(model.sim_dim()) + ")" +
                            (tables->bridged() ? " + Brownian bridge" : "") +
                            (tables->owen ? " + Owen scrambling" : "");
            }
            res.diagnostics = note + (req.antithetic ? " + antithetic" : "") + cv_note + cv_off +
                              (st.events_note.empty() ? "" : " + " + st.events_note);
            res.device = "gpu";
            res.gpus = gpu::gpus_used(req);
            return res;
        }
    } // namespace

    namespace
    {
        /// Why multilevel Monte Carlo cannot take this script (WP 26 P4c),
        /// empty if it can.
        std::string mlmc_unsuitable(const ScriptedProduct<Real> &product, const ISimulationModel<Real> &model)
        {
            if (product.has_exercise())
                return "early exercise";
            if (product.payoff_labels().size() != 1)
                return "several payoff labels (the estimator takes one)";
            if (!model.clone()->set_refinement(2))
                return "the model has no time-step bias to remove (or cannot refine its grid yet: MLMC runs "
                       "under local vol)";
            return {};
        }

        /**
         * One multilevel QMC level on the GPU (WP 26 P6, §10). The fine and
         * the coarse grids are two ordinary Sobol runs of the script's
         * kernel on the same points (mc::coupled_sobol_tables): replicate
         * b's sample of the correction is the difference of its two means.
         * More points are a run of the new points alone (the tables'
         * first_point, §10.3).
         */
        class GpuQmcLevel final : public IQmcLevel
        {
          public:
            /// What the levels of a run report of their launches.
            struct Note
            {
                int gpus = 0;
                std::string events; ///< compiled or interpreted, and why
            };

            /// The risks of a run's levels (§10.2): per level, each
            /// replicate's fine-minus-coarse risks, replicate-major. The
            /// run's levels are released with it; this outlives them.
            struct Risks
            {
                std::vector<std::vector<double>> levels;
                int replicates = 0;
            };

            /// With `risks`, every launch is the per-path adjoint instead
            /// of the price kernel (local vol), and the level's risks are
            /// kept there.
            GpuQmcLevel(const ScriptedProduct<Real> &product, const ISimulationModel<Real> &base, int level,
                        const MlqmcSettings &s, int max_gpus, Note &note, Risks *risks = nullptr)
                : product_(product), mats_(discount_lists(product)), devices_(gpu::devices_for(max_gpus)), note_(note), risks_(risks), level_(level)
            {
                if (risks_)
                {
                    risks_->replicates = s.replicates;
                    if (risks_->levels.size() <= static_cast<std::size_t>(level))
                        risks_->levels.resize(static_cast<std::size_t>(level) + 1);
                }
                fine_ = describe(base, level, dm_fine_);
                cost_ = static_cast<double>(fine_->sim_dim());
                std::vector<uint64_t> keys; // the CPU levels' keys (QmcLevel)
                for (int r = 0; r < s.replicates; ++r)
                    keys.push_back((s.seed << 32) ^ (static_cast<std::uint64_t>(level) << 16) ^
                                   static_cast<std::uint64_t>(r));
                if (level == 0)
                {
                    tables_.fine = mc::sobol_tables(fine_->sim_dim(), keys, fine_->brownian_layout(), true, s.owen);
                    return;
                }
                coarse_ = describe(base, level - 1, dm_coarse_);
                cost_ += static_cast<double>(coarse_->sim_dim());
                tables_ = mc::coupled_sobol_tables(fine_->brownian_layout(), coarse_->brownian_layout(), keys, s.owen);
            }

            /// Points points_ .. points_ + n - 1 of every replicate, folded
            /// into the level's means (and risks).
            void add(long long n) override
            {
                std::vector<double> *rows = risks_ ? &risks_->levels[static_cast<std::size_t>(level_)] : nullptr;
                std::vector<double> more_rows, coarse_rows;
                tables_.fine.first_point = tables_.coarse.first_point = static_cast<uint32_t>(points_);
                std::vector<double> more = run(dm_fine_, tables_.fine, n, rows ? &more_rows : nullptr);
                if (coarse_)
                {
                    const std::vector<double> coarse =
                        run(dm_coarse_, tables_.coarse, n, rows ? &coarse_rows : nullptr);
                    for (std::size_t r = 0; r < more.size(); ++r)
                        more[r] -= coarse[r];
                    for (std::size_t i = 0; i < coarse_rows.size(); ++i)
                        more_rows[i] -= coarse_rows[i];
                }
                // Each mean over all the points so far.
                const double had = static_cast<double>(points_), got = static_cast<double>(n);
                auto fold = [&](std::vector<double> &all, const std::vector<double> &extra)
                {
                    if (all.empty())
                        all.assign(extra.size(), 0.0);
                    for (std::size_t i = 0; i < all.size(); ++i)
                        all[i] = (had * all[i] + got * extra[i]) / (had + got);
                };
                fold(means_, more);
                if (rows)
                    fold(*rows, more_rows);
                points_ += n;
            }

            long long points() const override { return points_; }
            double cost() const override { return cost_; }
            double mean() const override { return mlqmc_detail::replicate_mean(means_); }
            double estimator_variance() const override { return mlqmc_detail::replicate_variance(means_); }

          private:
            /// The model of `level`'s fine grid, as the device steps it.
            std::unique_ptr<ISimulationModel<Real>> describe(const ISimulationModel<Real> &base, int level,
                                                             DeviceModel &dm) const
            {
                auto m = mlmc_detail::at_refinement(base, mlmc_detail::refinement_of(level), product_);
                if (!m->describe_device(dm))
                    throw InvalidInput("this model runs on the CPU only");
                if (risks_ && dm.kind != DeviceModel::Kind::LocalVol)
                    throw InvalidInput("multilevel risks run under local vol (the GPU's per-path adjoint)");
                if (m->sim_dim() > static_cast<std::size_t>(sobol_detail::kMaxDimension))
                    throw InvalidInput("multilevel QMC: level " + std::to_string(level) + " needs " +
                                       std::to_string(m->sim_dim()) + " draws per path, more than the " +
                                       std::to_string(sobol_detail::kMaxDimension) + " Sobol dimensions");
                const std::string why = gpu::script_gpu_unsupported(product_.program(), dm, mats_);
                if (!why.empty())
                    throw InvalidInput(why);
                return m;
            }

            /// Each replicate's mean over the `n` points from the tables'
            /// first_point; with `rows`, by the adjoint, and each
            /// replicate's risks there.
            std::vector<double> run(const DeviceModel &dm, const mc::SobolTables &tables, long long n,
                                    std::vector<double> *rows)
            {
                gpu::ScriptGpuRequest req;
                req.program = &product_.program();
                req.ir = &product_.ir();
                req.model = &dm;
                req.baseline = product_.baseline();
                req.discount_mats = mats_;
                req.antithetic = false;
                req.devices = devices_;
                req.n_units = static_cast<uint64_t>(n);
                req.sobol = &tables;
                note_.gpus = std::max(note_.gpus, gpu::gpus_used(req));
                if (rows)
                {
                    gpu::ScriptAdjointGpuResult r = gpu::simulate_script_adjoint(req);
                    *rows = std::move(r.replicate_risks);
                    return r.replicate_prices;
                }
                const gpu::ScriptGpuStats st = gpu::simulate_script(req);
                note_.events = st.events_note;
                std::vector<double> means;
                for (const WelfordAccumulator &w : st.plain)
                    means.push_back(w.mean);
                return means;
            }

            const ScriptedProduct<Real> &product_;
            std::vector<std::vector<Time>> mats_;
            std::vector<int> devices_;
            std::unique_ptr<ISimulationModel<Real>> fine_, coarse_;
            DeviceModel dm_fine_, dm_coarse_;
            mc::CoupledSobolTables tables_;
            std::vector<double> means_;
            long long points_ = 0;
            double cost_ = 0.0;
            Note &note_;
            Risks *risks_ = nullptr;
            int level_ = 0;
        };

        /// Points per replicate a GPU level starts with: a launch has a
        /// fixed cost that 16 points (the CPU's start) would not repay.
        constexpr long long kGpuMlqmcInitialPoints = 1024;

        SimulationMCResult run_mlmc(const ScriptedProduct<Real> &product, const ISimulationModel<Real> &model,
                                    const PricingSettings &settings, ThreadPool *pool)
        {
            const std::uint64_t seed = static_cast<std::uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
            // Sobol (the default sampler): multilevel QMC (WP 26 P5), 15-25x
            // cheaper than MLMC on the measured cases, its replicates on the
            // pool; MLMC when a level needs more draws than Sobol has
            // dimensions, or when pseudo-random draws were asked for.
            MlmcResult r;
            std::string sampler_note = " (CPU, pseudo-random)";
            bool done = false;
            std::string device = "cpu";
            int gpus = 0;
            if (settings.mc_sampler == SamplerKind::Sobol)
            {
                MlqmcSettings q;
                q.eps = settings.mc_mlmc_eps;
                q.rel_eps = settings.mc_mlmc_rel_eps;
                q.seed = seed;
                q.owen = settings.mc_sobol_owen;
                // On the GPU when asked for and the device can take the
                // script and the model (WP 26 P6); otherwise the CPU, saying
                // why.
                const bool explicit_gpu = settings.mc_device == ComputeDevice::Gpu;
                std::string gpu_off;
                if (explicit_gpu || (settings.mc_device == ComputeDevice::Auto && gpu::device_count() > 0))
                {
                    MlqmcSettings g = q;
                    g.initial_points = kGpuMlqmcInitialPoints;
                    GpuQmcLevel::Note note;
                    try
                    {
                        r = simulate_mlqmc_levels(
                            [&](int level) -> std::unique_ptr<IQmcLevel>
                            {
                                return std::make_unique<GpuQmcLevel>(product, model, level, g, settings.mc_gpus, note);
                            },
                            g);
                        done = true;
                        device = "gpu";
                        gpus = note.gpus;
                        const std::vector<int> all = gpu::devices_for(settings.mc_gpus);
                        sampler_note = " (GPU: " +
                                       gpu::devices_label(std::vector<int>(all.begin(), all.begin() + gpus)) +
                                       (note.events.empty() ? "" : ", " + note.events) + ")";
                    }
                    catch (const InvalidInput &e)
                    {
                        if (explicit_gpu)
                            throw InvalidInput(std::string("GPU: ") + e.what());
                        gpu_off = std::string("; not on the GPU: ") + e.what();
                    }
                }
                if (!done)
                    try
                    {
                        r = simulate_mlqmc(product, model, q, pool);
                        sampler_note = " (CPU" + gpu_off + ")";
                        done = true;
                    }
                    catch (const InvalidInput &e)
                    {
                        sampler_note = std::string(" (CPU, pseudo-random: ") + e.what() + gpu_off + ")";
                    }
            }
            if (!done)
            {
                MlmcSettings s;
                s.eps = settings.mc_mlmc_eps;
                s.seed = seed;
                if (settings.mc_mlmc_rel_eps > 0.0)
                {
                    // A relative target needs the price's scale: level 0's.
                    const MlmcResult pilot = simulate_mlmc_fixed(product, model, {4000}, seed);
                    s.eps = std::max(s.eps, settings.mc_mlmc_rel_eps * std::fabs(pilot.value));
                }
                r = simulate_mlmc(product, model, s);
            }
            SimulationMCResult res;
            res.labels = product.payoff_labels();
            res.values = {r.value};
            res.std_errors = {r.std_error};
            res.discretisation_bias = r.bias;
            res.n_paths = 0;
            for (const MlmcLevel &l : r.levels)
                res.n_paths += l.n;
            std::ostringstream os;
            os << "SimulationMCEngine, " << r.diagnostics << "; target RMS error " << r.eps
               << ", estimated remaining bias " << r.bias << sampler_note;
            res.diagnostics = os.str();
            res.device = device;
            res.gpus = gpus;
            return res;
        }

        /// A plain run with its discretisation bias (WP 26 §9.4): a model
        /// that refines its grid is priced with every step halved, the bias
        /// estimated as the difference from the unhalved run on the same
        /// seed (weak order one: bias(h/2) ~ P(h/2) - P(h)); a model exact
        /// between the product's dates (exact_between_dates) has bias 0.
        SimulationMCResult run_with_bias(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                         const PricingSettings &settings, ThreadPool *pool)
        {
            PricingSettings plain = settings;
            plain.mc_bias_estimate = false;
            std::unique_ptr<ISimulationModel<Real>> halved = model.clone();
            if (!product.has_exercise() && halved->set_refinement(2))
            {
                SimulationMCResult fine = simulate_script(product, *halved, plain, pool);
                const SimulationMCResult coarse = simulate_script(product, model, plain, pool);
                fine.discretisation_bias = std::fabs(fine.npv() - coarse.npv());
                fine.diagnostics += " | steps halved, discretisation bias estimated from the unhalved run";
                return fine;
            }
            SimulationMCResult res = simulate_script(product, model, plain, pool);
            if (model.exact_between_dates())
                res.discretisation_bias = 0.0;
            else
                res.diagnostics += " | discretisation bias not estimated (the model cannot refine its grid)";
            return res;
        }
    } // namespace

    SimulationMCResult simulate_script(const ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                       const PricingSettings &settings, ThreadPool *pool)
    {
        // Multilevel Monte Carlo (WP 26 P4c, §9.4): when the script suits
        // it, otherwise the usual route, saying why.
        if (settings.mc_mlmc_eps < 0.0 || settings.mc_mlmc_rel_eps < 0.0)
            throw InvalidInput("mc_mlmc_eps and mc_mlmc_rel_eps must be >= 0 (0: no multilevel Monte Carlo)");
        if (settings.mc_mlmc_rel_eps > 0.0 && !(settings.mc_mlmc_eps > 0.0))
            throw InvalidInput("a relative multilevel target (mc_mlmc_rel_eps) needs mc_mlmc_eps > 0 as its floor");
        if (settings.mc_mlmc_eps > 0.0)
        {
            const std::string why = mlmc_unsuitable(product, model);
            if (why.empty())
                return run_mlmc(product, model, settings, pool);
            PricingSettings plain = settings;
            plain.mc_mlmc_eps = 0.0;
            plain.mc_mlmc_rel_eps = 0.0;
            SimulationMCResult res = simulate_script(product, model, plain, pool);
            res.diagnostics += " (no multilevel Monte Carlo: " + why + ")";
            return res;
        }
        if (settings.mc_bias_estimate)
            return run_with_bias(product, model, settings, pool);

        const bool explicit_gpu = settings.mc_device == ComputeDevice::Gpu;
        const bool want_gpu = explicit_gpu || (settings.mc_device == ComputeDevice::Auto && gpu::device_count() > 0);
        if (!want_gpu)
            return simulate<Real>(product, model, settings, pool);

        // Why the GPU cannot take this request, if it cannot.
        std::string why;
        DeviceModel dm;
        std::vector<std::vector<Time>> mats;
        model.init(product.timeline(), product.defline());
        if (product.has_exercise())
            why = "early exercise (exercise() / call()) runs on the CPU only";
        else if (!model.describe_device(dm))
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
            SimulationMCResult res = simulate<Real>(product, model, settings, pool);
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
            why = "this model's risks run on the CPU only (the GPU differentiates Black-Scholes, Heston, local "
                  "vol and SLV)";
            return std::nullopt;
        }
        const auto mats = discount_lists(product);
        why = gpu::script_gpu_unsupported(product.program(), dm, mats);
        if (!why.empty())
            return std::nullopt;
        // A model with a grid (every sigma_loc point, every leverage point)
        // takes the adjoint; the others the tangent, a pass per parameter.
        // Both are generated from the script's IR (WP 27, lots L3-L5).
        const bool adjoint = dm.kind == DeviceModel::Kind::LocalVol || dm.kind == DeviceModel::Kind::SLV;
        const int dirs = mc::dual_directions(dm.kind, dm.n_assets);
        if (dm.kind == DeviceModel::Kind::Heston && dm.jump_k != 0.0)
        {
            why = "jump parameters are set: the GPU does not differentiate the jump compensator";
            return std::nullopt;
        }
        if (!adjoint && dirs <= 0)
        {
            why = "this model's risks run on the CPU only";
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
        req.ir = &product.ir();
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
            req.n_units = static_cast<uint64_t>(
                sobol_points_per_replicate(n_paths, static_cast<std::size_t>(kAadRqmcReplicates)));
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
                                  (sobol ? " risks)" : " risks, batches of 512)") + draws + " + " + r.note;
            }
            else
            {
                std::string how;
                const auto reps = gpu::simulate_script_duals(req, &how);
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
                res.diagnostics = "forward-mode risks on GPU (" + card + ", " + std::to_string(dirs) +
                                  " directions)" + draws + " + " + how;
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
        catch (const InvalidInput &e) // what only the compiled kernels cover, with compilation off
        {
            why = e.what();
            return std::nullopt;
        }
    }

    std::optional<AADSimulResults> simulate_script_aad_mlqmc_gpu(const ScriptedProduct<Real> &product,
                                                                 const ISimulationModel<Real> &model, double eps,
                                                                 double rel_eps, std::uint64_t seed,
                                                                 std::string &why, int max_gpus)
    {
        if (gpu::device_count() == 0)
        {
            why = "this server has no usable CUDA device";
            return std::nullopt;
        }
        why = mlmc_unsuitable(product, model);
        if (!why.empty())
            return std::nullopt;

        MlqmcSettings s;
        s.eps = eps;
        s.rel_eps = rel_eps;
        s.seed = seed;
        s.initial_points = kGpuMlqmcInitialPoints;
        GpuQmcLevel::Note note;
        GpuQmcLevel::Risks risks;
        MlmcResult r;
        try
        {
            r = simulate_mlqmc_levels(
                [&](int level) -> std::unique_ptr<IQmcLevel>
                { return std::make_unique<GpuQmcLevel>(product, model, level, s, max_gpus, note, &risks); },
                s);
        }
        catch (const InvalidInput &e)
        {
            why = e.what();
            return std::nullopt;
        }
        catch (const gpu::GpuUnavailable &e)
        {
            why = e.what();
            return std::nullopt;
        }

        AADSimulResults res;
        res.price = r.value;
        res.price_std_error = r.std_error;
        res.discretisation_bias = r.bias;
        res.gpus = note.gpus;
        for (const MlmcLevel &l : r.levels)
            res.n_paths += l.n;
        {
            auto m = model.clone();
            m->init(product.timeline(), product.defline());
            res.risk_labels = m->parameter_labels();
        }
        // A risk is the sum of its levels' estimates, each the mean of the
        // level's replicates; its variance the sum of theirs.
        const std::size_t np = res.risk_labels.size(), R = static_cast<std::size_t>(risks.replicates);
        res.risks.assign(np, 0.0);
        std::vector<double> var(np, 0.0), column(R);
        for (const std::vector<double> &rows : risks.levels)
            for (std::size_t p = 0; p < np && rows.size() == R * np; ++p)
            {
                for (std::size_t b = 0; b < R; ++b)
                    column[b] = rows[b * np + p];
                res.risks[p] += mlqmc_detail::replicate_mean(column);
                var[p] += mlqmc_detail::replicate_variance(column);
            }
        for (const double v : var)
            res.risk_std_errors.push_back(std::sqrt(v));

        const std::vector<int> all = gpu::devices_for(max_gpus);
        std::ostringstream os;
        os << r.diagnostics << "; target RMS error " << r.eps << ", estimated remaining bias " << r.bias
           << "; risks by the per-path adjoint of every level (" << np << " risks, GPU: "
           << gpu::devices_label(std::vector<int>(all.begin(), all.begin() + note.gpus)) << ")";
        res.diagnostics = os.str();
        return res;
    }

} // namespace quantModeling
