#include "quantModeling/engines/xva/exposure_engine.hpp"

#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/gpu/exposure.hpp"
#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <string>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;
        /// Paths per task. Fixed, so that the split never depends on the
        /// number of threads (the results would not either way: every path
        /// writes its own row).
        constexpr std::size_t kChunk = 256;
    } // namespace

    std::vector<Time> exposure_grid(Time horizon, const std::vector<Time> &event_times,
                                    const ExposureGridSettings &settings)
    {
        if (!(horizon > kTimeEps) || !std::isfinite(horizon))
            throw InvalidInput("exposure grid: the horizon must be finite and > 0");
        if (!(settings.weekly_until >= 0.0) || !(settings.monthly_until >= settings.weekly_until))
            throw InvalidInput("exposure grid: need 0 <= weekly_until <= monthly_until");
        if (!(settings.margin_period_of_risk >= 0.0))
            throw InvalidInput("exposure grid: the margin period of risk must be >= 0");

        std::vector<Time> times;
        const auto add_regular = [&](Real steps_per_year, Time from, Time until)
        {
            const Time end = std::min(until, horizon);
            for (long k = 1;; ++k)
            {
                const Time t = static_cast<Real>(k) / steps_per_year;
                if (t > end + kTimeEps)
                    break;
                // `from` itself is kept: without it the step across the
                // boundary would be longer than either regime allows.
                if (t > from - kTimeEps)
                    times.push_back(t);
            }
        };
        add_regular(52.0, 0.0, settings.weekly_until);
        add_regular(12.0, settings.weekly_until, settings.monthly_until);
        add_regular(4.0, settings.monthly_until, horizon);
        for (const Time t : event_times)
            if (t > kTimeEps && t <= horizon + kTimeEps)
                times.push_back(std::min(t, horizon));
        times.push_back(horizon);

        const auto sorted_unique = [](std::vector<Time> dates)
        {
            std::sort(dates.begin(), dates.end());
            std::vector<Time> unique;
            for (const Time t : dates)
                if (unique.empty() || t - unique.back() > kTimeEps)
                    unique.push_back(t);
            return unique;
        };
        std::vector<Time> grid = sorted_unique(std::move(times));
        if (settings.margin_period_of_risk > kTimeEps)
        {
            std::vector<Time> with_lags = grid;
            for (const Time t : grid)
                if (t - settings.margin_period_of_risk > kTimeEps)
                    with_lags.push_back(t - settings.margin_period_of_risk);
            grid = sorted_unique(std::move(with_lags));
        }
        return grid;
    }

    HullWhiteExposureEngine::HullWhiteExposureEngine(const HullWhiteCurveModel &model)
        : model_(model)
    {
    }

    std::size_t HullWhiteExposureEngine::add(const Instrument &instrument, Real quantity)
    {
        return add(make_hull_white_future_value(instrument, model_), quantity);
    }

    std::size_t HullWhiteExposureEngine::add(std::unique_ptr<FutureValue> trade, Real quantity)
    {
        if (!trade)
            throw InvalidInput("exposure engine: null trade");
        if (!std::isfinite(quantity))
            throw InvalidInput("exposure engine: the quantity must be finite");
        trades_.push_back({std::move(trade), quantity});
        return trades_.size() - 1;
    }

    std::vector<Time> HullWhiteExposureEngine::grid(const ExposureGridSettings &settings) const
    {
        if (trades_.empty())
            throw InvalidInput("exposure engine: no trade");
        Time horizon = 0.0;
        std::vector<Time> events;
        for (const Trade &trade : trades_)
        {
            horizon = std::max(horizon, trade.value->maturity());
            const std::vector<Time> times = trade.value->event_times();
            events.insert(events.end(), times.begin(), times.end());
        }
        return exposure_grid(horizon, events, settings);
    }

    HullWhiteExposureEngine::Dynamics HullWhiteExposureEngine::prepare(
        const ExposureSimulationSettings &settings, ExposurePaths &out)
    {
        if (settings.paths == 0)
            throw InvalidInput("exposure engine: at least one path is required");
        out.times = grid(settings.grid);
        out.paths = settings.paths;
        const std::size_t n = out.times.size();

        for (Trade &trade : trades_)
            trade.value->bind(out.times);

        // Exact transitions of x under the T*-forward measure, and the
        // discount weight P(0, T*) / P(t, T* | x) = P(0, t) exp(G x + G² y / 2),
        // G = G(t, T*).
        const Time horizon = out.times.back();
        // f(0, t), today's instantaneous forward rate: the slope of -ln P(0, ·).
        const auto forward = [this](Time t)
        {
            constexpr Time h = 1e-4;
            const Time lo = std::max(t - h, 0.0), hi = t + h;
            const Real f =
                std::log(model_.discount().discount(lo) / model_.discount().discount(hi)) / (hi - lo);
            // A curve whose discount factor jumps (held flat before its first
            // pillar, for instance) has no forward rate there: the short rate
            // would start from thousands of percent.
            if (!std::isfinite(f) || std::abs(f) > 1.0)
                throw InvalidInput(
                    "exposure engine: the discount curve has no finite forward rate at t=" +
                    std::to_string(t) +
                    " (a jump in the discount factor); build it with a flat-forward extrapolation");
            return f;
        };
        if (settings.historical)
        {
            settings.historical->validate();
            out.measure = ExposureMeasure::Historical;
        }
        // The pricing measure's transitions: those of the main simulation
        // unless it is historical, and always those of the pilot.
        Dynamics dynamics;
        std::vector<Real> &q_decay = dynamics.pricing.decay, &q_drift = dynamics.pricing.drift,
                          &q_sd = dynamics.pricing.sd, &q_scale = dynamics.pricing.weight_scale,
                          &q_G = dynamics.pricing.weight_G;
        for (std::vector<Real> *v : {&q_decay, &q_drift, &q_sd, &q_scale, &q_G})
            v->resize(n);
        Time previous = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Time t = out.times[i];
            const auto tr = model_.transition(previous, t, horizon);
            q_decay[i] = tr.decay;
            q_drift[i] = tr.drift;
            q_sd[i] = std::sqrt(tr.variance);
            q_G[i] = model_.G(t, horizon);
            q_scale[i] = model_.discount().discount(t) * std::exp(0.5 * q_G[i] * q_G[i] * model_.y(t));
            previous = t;
        }
        dynamics.main = dynamics.pricing;
        std::vector<Real> &decay = dynamics.main.decay, &drift = dynamics.main.drift,
                          &sd = dynamics.main.sd, &weight_scale = dynamics.main.weight_scale,
                          &weight_G = dynamics.main.weight_G;
        previous = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Time t = out.times[i];
            if (settings.historical)
            {
                // The short rate is an Ornstein-Uhlenbeck process under the
                // historical measure, with an exact Gaussian transition:
                //   r(t) = θ + (r(s) - θ) b + σ sqrt((1 - b²) / (2a)) Z,
                //   b = exp(-a (t - s)).
                // The model's state is x = r - f(0, ·), so
                //   x(t) = b x(s) + [b f(0, s) + θ (1 - b) - f(0, t)] + ...
                // and x(0) = 0: the rate starts from today's short rate.
                const HistoricalRateDynamics &h = *settings.historical;
                const Real b = std::exp(-h.mean_reversion * (t - previous));
                decay[i] = b;
                drift[i] = b * forward(previous) + h.long_run_rate * (1.0 - b) - forward(t);
                sd[i] = h.sigma * std::sqrt((1.0 - b * b) / (2.0 * h.mean_reversion));
                // Equally likely paths, nothing discounted.
                out.discount.push_back(1.0);
                weight_G[i] = 0.0;
                weight_scale[i] = 1.0;
            }
            else
            {
                out.discount.push_back(model_.discount().discount(t));
            }
            previous = t;
        }
        return dynamics;
    }

    bool HullWhiteExposureEngine::on_device(const ExposureSimulationSettings &settings,
                                            xva::ExposureProgram &program, std::string &note) const
    {
        if (settings.device == ComputeDevice::Cpu)
            return false;
        std::string why;
        if (gpu::device_count() == 0)
            why = gpu::compiled_with_cuda()
                      ? "this server has no usable CUDA device"
                      : "the pricing library was built without the CUDA backend";
        else if (settings.simm)
            why = "SIMM on every path is computed on the CPU";
        else
        {
            program = {};
            for (std::size_t k = 0; k < trades_.size() && why.empty(); ++k)
            {
                if (trades_[k].value->compile(program))
                    program.trades.back().quantity = trades_[k].quantity;
                else
                    why = "trade " + std::to_string(k) +
                          " is valued by regression, which runs on the CPU";
            }
        }
        if (why.empty())
            return true;
        if (settings.device == ComputeDevice::Gpu)
        {
            if (gpu::device_count() == 0)
                throw gpu::GpuUnavailable("exposure engine: GPU requested, but " + why);
            throw InvalidInput("exposure engine: GPU requested, but " + why);
        }
        note = why;
        return false;
    }

    ExposurePaths HullWhiteExposureEngine::simulate(const ExposureSimulationSettings &settings,
                                                    ThreadPool *pool)
    {
        ExposurePaths out;
        const Dynamics dynamics = prepare(settings, out);
        const std::vector<Real> &q_decay = dynamics.pricing.decay, &q_drift = dynamics.pricing.drift,
                                &q_sd = dynamics.pricing.sd, &q_scale = dynamics.pricing.weight_scale,
                                &q_G = dynamics.pricing.weight_G;
        const std::vector<Real> &decay = dynamics.main.decay, &drift = dynamics.main.drift,
                                &sd = dynamics.main.sd, &weight_scale = dynamics.main.weight_scale,
                                &weight_G = dynamics.main.weight_G;
        const std::size_t n = out.times.size();
        const std::size_t N = settings.paths;
        const std::size_t K = trades_.size();

        // (trades [× 2 with cash flows] + the discount weights [+ SIMM])
        // matrices of doubles.
        const std::size_t matrices = K * (settings.keep_cashflows ? 2 : 1) + 1 + (settings.simm ? 1 : 0);
        const std::size_t limit_cells = settings.memory_limit_bytes / sizeof(Real);
        if (N > limit_cells / n / matrices)
            throw InvalidInput(
                "exposure engine: " + std::to_string(K) + " trades x " + std::to_string(N) +
                " paths x " + std::to_string(n) + " dates exceed the memory limit of " +
                std::to_string(settings.memory_limit_bytes >> 20) +
                " MiB; reduce the paths or raise memory_limit_bytes");

        // ── Pilot: fit the trades valued by regression (lot X4) ──────────
        pilot_dispersion_ = 0.0;
        const bool regression =
            std::any_of(trades_.begin(), trades_.end(),
                        [](const Trade &trade)
                        { return trade.value->needs_pilot(); });
        if (regression)
        {
            PilotPaths pilot;
            pilot.paths = settings.pilot_paths > 0
                              ? settings.pilot_paths
                              : std::clamp<std::size_t>(4 * N, 20000, 200000);
            if (pilot.paths > (limit_cells / n - std::min(limit_cells / n, N * matrices)) / 2)
                throw InvalidInput(
                    "exposure engine: the pilot of " + std::to_string(pilot.paths) +
                    " paths does not fit in what the cube leaves of the memory limit; reduce "
                    "the paths or raise memory_limit_bytes");
            pilot.times = out.times;
            if (settings.pilot_dispersion)
            {
                if (!(*settings.pilot_dispersion >= 0.0) || !std::isfinite(*settings.pilot_dispersion))
                    throw InvalidInput("exposure engine: the pilot dispersion must be finite and >= 0");
                pilot_dispersion_ = *settings.pilot_dispersion;
            }
            else if (settings.historical)
            {
                // Where the historical scenarios go, the pricing measure's
                // paths may not: start the pilot from a dispersed state
                // x(0) ~ N(0, s0²), s0 the smallest for which, at every date,
                // three standard deviations of the pilot cover four of the
                // historical scenarios around their own mean. The conditional
                // law of the future given x(t_i) is untouched, and it is all
                // a regression estimates.
                Real mean_q = 0.0, var_q = 0.0, carried = 1.0, mean_p = 0.0, var_p = 0.0, s0_sq = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    mean_q = q_decay[i] * mean_q + q_drift[i];
                    var_q = q_decay[i] * q_decay[i] * var_q + q_sd[i] * q_sd[i];
                    carried *= q_decay[i];
                    mean_p = decay[i] * mean_p + drift[i];
                    var_p = decay[i] * decay[i] * var_p + sd[i] * sd[i];
                    const Real needed = (std::abs(mean_p - mean_q) + 4.0 * std::sqrt(var_p)) / 3.0;
                    s0_sq = std::max(s0_sq, (needed * needed - var_q) / (carried * carried));
                }
                pilot_dispersion_ = std::sqrt(std::max(s0_sq, 0.0));
            }
            pilot.state.resize(pilot.paths * n);
            pilot.deflator.resize(pilot.paths * n);
            // A stream the main simulation never draws: the seed's complement.
            PhiloxGaussianSource gaussian(~settings.seed);
            for (std::size_t p = 0; p < pilot.paths; ++p)
            {
                gaussian.set_path(p);
                Real x = pilot_dispersion_ * gaussian.next();
                for (std::size_t i = 0; i < n; ++i)
                {
                    x = q_decay[i] * x + q_drift[i] + q_sd[i] * gaussian.next();
                    pilot.state[p * n + i] = x;
                    pilot.deflator[p * n + i] = q_scale[i] * std::exp(q_G[i] * x);
                }
            }
            for (Trade &trade : trades_)
                if (trade.value->needs_pilot())
                    trade.value->fit(pilot);
            out.pilot_paths = pilot.paths;
        }
        for (const Trade &trade : trades_)
            out.trade_values_today.push_back(trade.quantity * trade.value->value_today());

        // SIMM from the exposures the trades report: a cash flow worth
        // `amount` at tenor τ loses τ × amount × 1 bp when its zero rate
        // rises by a basis point.
        const auto margin_of = [&settings](const std::vector<BondExposure> &bonds,
                                           const std::vector<VolExposure> &vols, Time t)
        {
            simm::Sensitivities s;
            for (const BondExposure &b : bonds)
                if (b.maturity > t)
                    s.add_delta(b.maturity - t, -(b.maturity - t) * b.amount * 1e-4);
            for (const VolExposure &v : vols)
                if (v.expiry > t)
                    s.add_vega(v.expiry - t, v.vega_times_vol);
            return simm::interest_rate_margin(s, settings.simm_currency);
        };
        // What each trade reports is for one unit of it.
        const auto scale = [](std::vector<BondExposure> &bonds, std::vector<VolExposure> &vols,
                              std::size_t from_bond, std::size_t from_vol, Real quantity)
        {
            for (std::size_t j = from_bond; j < bonds.size(); ++j)
                bonds[j].amount *= quantity;
            for (std::size_t j = from_vol; j < vols.size(); ++j)
                vols[j].vega_times_vol *= quantity;
        };
        if (settings.simm)
        {
            std::vector<BondExposure> bonds;
            std::vector<VolExposure> vols;
            for (std::size_t k = 0; k < K; ++k)
            {
                const std::size_t nb = bonds.size(), nv = vols.size();
                if (!trades_[k].value->sensitivities_today(bonds, vols))
                    throw InvalidInput(
                        "exposure engine: trade " + std::to_string(k) +
                        " is valued by regression and cannot give the sensitivities SIMM needs; "
                        "SIMM per path covers swaps and European swaptions");
                scale(bonds, vols, nb, nv, trades_[k].quantity);
            }
            out.simm_today = margin_of(bonds, vols, 0.0);
            out.simm.resize(N * n);
        }

        out.discount_weight.resize(N * n);
        out.trade_values.assign(K, std::vector<Real>(N * n));
        if (settings.keep_cashflows)
            out.trade_cashflows.assign(K, std::vector<Real>(N * n));

        xva::ExposureProgram program;
        if (on_device(settings, program, out.device_note))
        {
            gpu::ExposureGpuRequest request;
            request.program = &program;
            request.dynamics = &dynamics.main;
            request.seed = settings.seed;
            request.paths = N;
            request.devices = gpu::devices_for(settings.max_gpus);
            request.max_blocks_per_launch = settings.gpu_blocks_per_launch;
            gpu::ExposureCubeTarget target;
            target.discount_weight = out.discount_weight.data();
            for (std::size_t k = 0; k < K; ++k)
            {
                target.values.push_back(out.trade_values[k].data());
                if (settings.keep_cashflows)
                    target.cashflows.push_back(out.trade_cashflows[k].data());
            }
            try
            {
                out.gpus = gpu::simulate_exposure_cube(request, target);
                out.device = "gpu";
                return out;
            }
            catch (const gpu::GpuUnavailable &e)
            {
                // A card that fails mid-run (its memory is shared) must not
                // fail a simulation that was only allowed to use it.
                if (settings.device == ComputeDevice::Gpu)
                    throw;
                out.device_note = e.what();
            }
        }

        const auto run_chunk = [&](std::size_t first, std::size_t last)
        {
            std::vector<Real> state(n), values(n), flows(n);
            std::vector<BondExposure> bonds;
            std::vector<VolExposure> vols;
            // This task's scratch for the trades that value a path
            // sequentially.
            std::vector<std::unique_ptr<FutureValue::Workspace>> workspaces;
            workspaces.reserve(K);
            for (const Trade &trade : trades_)
                workspaces.push_back(trade.value->make_workspace());
            PhiloxGaussianSource gaussian(settings.seed);
            for (std::size_t p = first; p < last; ++p)
            {
                gaussian.set_path(p);
                Real x = 0.0;
                const std::size_t row = p * n;
                for (std::size_t i = 0; i < n; ++i)
                {
                    x = decay[i] * x + drift[i] + sd[i] * gaussian.next();
                    state[i] = x;
                    out.discount_weight[row + i] = weight_scale[i] * std::exp(weight_G[i] * x);
                }
                for (std::size_t k = 0; k < K; ++k)
                {
                    const Real quantity = trades_[k].quantity;
                    trades_[k].value->evaluate_path(state.data(), n, values.data(),
                                                    settings.keep_cashflows ? flows.data() : nullptr,
                                                    workspaces[k].get());
                    std::vector<Real> &stored = out.trade_values[k];
                    for (std::size_t i = 0; i < n; ++i)
                        stored[row + i] = quantity * values[i];
                    if (settings.keep_cashflows)
                    {
                        std::vector<Real> &stored_flows = out.trade_cashflows[k];
                        for (std::size_t i = 0; i < n; ++i)
                            stored_flows[row + i] = quantity * flows[i];
                    }
                }
                if (settings.simm)
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        bonds.clear();
                        vols.clear();
                        for (std::size_t k = 0; k < K; ++k)
                        {
                            const std::size_t nb = bonds.size(), nv = vols.size();
                            trades_[k].value->sensitivities(i, state.data(), bonds, vols);
                            scale(bonds, vols, nb, nv, trades_[k].quantity);
                        }
                        out.simm[row + i] = margin_of(bonds, vols, out.times[i]).total();
                    }
            }
            return true;
        };

        if (pool == nullptr || pool->num_threads() == 0)
        {
            run_chunk(0, N);
            return out;
        }
        std::vector<TaskHandle> handles;
        handles.reserve(N / kChunk + 1);
        for (std::size_t first = 0; first < N; first += kChunk)
        {
            const std::size_t last = std::min(first + kChunk, N);
            handles.push_back(pool->spawn_task([&run_chunk, first, last]
                                               { return run_chunk(first, last); }));
        }
        // Wait for every task before leaving, even if one failed: they all
        // write into `out`.
        std::exception_ptr failure;
        for (TaskHandle &h : handles)
        {
            try
            {
                pool->active_wait(h);
            }
            catch (...)
            {
                if (!failure)
                    failure = std::current_exception();
            }
        }
        if (failure)
            std::rethrow_exception(failure);
        return out;
    }

    NettingSetExposure HullWhiteExposureEngine::simulate_netting_set(ExposureSimulationSettings settings,
                                                                     const NettingSetRequest &request,
                                                                     ThreadPool *pool)
    {
        if (trades_.empty())
            throw InvalidInput("exposure engine: no trade");
        std::vector<std::size_t> set = request.trades;
        if (set.empty())
        {
            set.resize(trades_.size());
            std::iota(set.begin(), set.end(), std::size_t{0});
        }
        std::vector<bool> seen(trades_.size(), false);
        for (const std::size_t k : set)
        {
            if (k >= trades_.size())
                throw InvalidInput("exposure engine: trade index out of range");
            if (seen[k])
                throw InvalidInput("exposure engine: a trade is netted twice");
            seen[k] = true;
        }
        const CollateralSettings &collateral = request.collateral;
        if (!collateral.initial_margin_received_paths.empty() ||
            !collateral.initial_margin_posted_paths.empty())
            throw InvalidInput("exposure engine: an initial margin that depends on the path needs "
                               "the cube; use simulate(), then collateralise()");
        const bool margined = !collateral.initial_margin_received.empty() ||
                              !collateral.initial_margin_posted.empty();
        if (margined && !request.csa)
            throw InvalidInput("exposure engine: initial margin needs a CSA");
        if (settings.simm)
            throw InvalidInput("exposure engine: SIMM on every path needs the cube; use simulate()");
        if (request.csa)
        {
            request.csa->validate();
            settings.grid.margin_period_of_risk = request.csa->margin_period_of_risk;
        }

        NettingSetExposure result;
        result.paths = settings.paths;
        std::vector<Real> on_positive, on_negative;
        const auto ask_weights = [&](const std::vector<Time> &times)
        {
            if (!request.weights)
                return;
            request.weights(times, on_positive, on_negative);
            if (on_positive.size() != times.size() || on_negative.size() != times.size())
                throw InvalidInput("exposure engine: the weights need one value per reporting date");
        };

        if (settings.device != ComputeDevice::Cpu)
        {
            ExposurePaths head;
            const Dynamics dynamics = prepare(settings, head);
            xva::ExposureProgram program;
            if (on_device(settings, program, result.device_note))
            {
                const std::size_t n = head.times.size();
                Real value_today = 0.0;
                for (const std::size_t k : set)
                    value_today += trades_[k].quantity * trades_[k].value->value_today();

                gpu::NettingSetGpuRequest netting;
                for (const std::size_t k : set)
                    netting.trades.push_back(static_cast<int>(k));
                netting.collateralised = request.csa.has_value();
                if (request.csa)
                    netting.plan =
                        collateral_plan(head.times, *request.csa, value_today, collateral.cashflows);
                else
                {
                    netting.plan.reporting.resize(n);
                    std::iota(netting.plan.reporting.begin(), netting.plan.reporting.end(), 0);
                }
                const std::size_t m = netting.plan.reporting.size();
                for (const std::vector<Real> *profile :
                     {&collateral.initial_margin_received, &collateral.initial_margin_posted})
                {
                    if (!profile->empty() && profile->size() != m)
                        throw InvalidInput("collateral: the initial margin profile needs one value "
                                           "per reporting date");
                    for (const Real im : *profile)
                        if (!(im >= 0.0))
                            throw InvalidInput("collateral: initial margin must be >= 0");
                }
                netting.initial_margin_received = collateral.initial_margin_received;
                netting.initial_margin_posted = collateral.initial_margin_posted;

                ExposureStatistics &s = result.statistics;
                s.measure = head.measure;
                s.trades = set;
                std::vector<Real> discount;
                for (const int i : netting.plan.reporting)
                {
                    s.times.push_back(head.times[static_cast<std::size_t>(i)]);
                    discount.push_back(head.discount[static_cast<std::size_t>(i)]);
                }
                ask_weights(s.times);
                netting.positive_weights = on_positive;
                netting.negative_weights = on_negative;

                gpu::ExposureGpuRequest run;
                run.program = &program;
                run.dynamics = &dynamics.main;
                run.seed = settings.seed;
                run.paths = settings.paths;
                run.devices = gpu::devices_for(settings.max_gpus);
                run.max_blocks_per_launch = settings.gpu_blocks_per_launch;
                std::optional<gpu::NettingSetGpuProfile> ran;
                try
                {
                    ran = gpu::simulate_netting_set(run, netting);
                }
                catch (const gpu::GpuUnavailable &e)
                {
                    if (settings.device == ComputeDevice::Gpu)
                        throw;
                    result.device_note = e.what();
                }
                if (ran)
                {
                    const gpu::NettingSetGpuProfile &profile = *ran;
                    const auto margin = [](const std::vector<Real> &im)
                    { return im.empty() ? 0.0 : im.front(); };
                    s.value_today =
                        request.csa ? net_of_initial_margin(value_today - netting.plan.held_today -
                                                                request.csa->independent_amount,
                                                            margin(collateral.initial_margin_received),
                                                            margin(collateral.initial_margin_posted))
                                    : value_today;
                    for (std::size_t r = 0; r < m; ++r)
                    {
                        const gpu::ExposureDateStats &d = profile.dates[r];
                        s.discounted_ee.push_back(d.positive.mean);
                        s.discounted_ene.push_back(d.negative.mean);
                        s.discounted_efv.push_back(d.value.mean);
                        s.discounted_ee_error.push_back(d.positive.std_error());
                        s.discounted_ene_error.push_back(d.negative.std_error());
                        s.discounted_efv_error.push_back(d.value.std_error());
                        s.ee.push_back(d.positive.mean / discount[r]);
                        s.ene.push_back(d.negative.mean / discount[r]);
                        s.efv.push_back(d.value.mean / discount[r]);
                    }
                    s.epe = expected_positive_exposure(s.times, s.ee);
                    s.eepe = effective_expected_positive_exposure(s.times, s.ee);
                    result.weighted_positive = {profile.weighted.positive.mean,
                                                profile.weighted.positive.std_error()};
                    result.weighted_negative = {profile.weighted.negative.mean,
                                                profile.weighted.negative.std_error()};
                    result.device = "gpu";
                    result.gpus = profile.gpus;
                    return result;
                }
                result.statistics = {};
            }
        }

        // On the CPU: the cube, then the post-processing of risk/.
        settings.device = ComputeDevice::Cpu;
        settings.keep_cashflows = request.csa && collateral.cashflows != MarginPeriodCashflows::Paid;
        const ExposurePaths cube = simulate(settings, pool);
        ExposurePaths netted;
        if (request.csa)
            netted = collateralise(cube, *request.csa, set, collateral);
        else
        {
            netted.paths = cube.paths;
            netted.measure = cube.measure;
            netted.times = cube.times;
            netted.discount = cube.discount;
            netted.discount_weight = cube.discount_weight;
            netted.trade_values.assign(1, std::vector<Real>(cube.paths * cube.dates(), 0.0));
            Real value_today = 0.0;
            for (const std::size_t k : set)
            {
                value_today += cube.trade_values_today[k];
                const std::vector<Real> &values = cube.trade_values[k];
                for (std::size_t j = 0; j < values.size(); ++j)
                    netted.trade_values[0][j] += values[j];
            }
            netted.trade_values_today = {value_today};
        }
        result.statistics = exposure_statistics(netted);
        result.statistics.trades = set;
        ask_weights(netted.times);
        if (request.weights)
        {
            const std::size_t m = netted.dates();
            WelfordAccumulator positive, negative;
            for (std::size_t p = 0; p < netted.paths; ++p)
            {
                Real gain = 0.0, loss = 0.0;
                for (std::size_t r = 0; r < m; ++r)
                {
                    const Real v = netted.discount_weight[p * m + r] * netted.trade_values[0][p * m + r];
                    if (v > 0.0)
                        gain += on_positive[r] * v;
                    if (v < 0.0)
                        loss += on_negative[r] * v;
                }
                positive.add(gain);
                negative.add(loss);
            }
            result.weighted_positive = {positive.mean, positive.std_error()};
            result.weighted_negative = {negative.mean, negative.std_error()};
        }
        return result;
    }

} // namespace quantModeling
