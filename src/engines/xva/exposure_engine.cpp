#include "quantModeling/engines/xva/exposure_engine.hpp"

#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
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

    ExposurePaths HullWhiteExposureEngine::simulate(const ExposureSimulationSettings &settings,
                                                    ThreadPool *pool)
    {
        if (settings.paths == 0)
            throw InvalidInput("exposure engine: at least one path is required");

        ExposurePaths out;
        out.times = grid(settings.grid);
        out.paths = settings.paths;
        const std::size_t n = out.times.size();
        const std::size_t N = settings.paths;
        const std::size_t K = trades_.size();

        // (trades [× 2 with cash flows] + the discount weights) matrices of doubles.
        const std::size_t matrices = K * (settings.keep_cashflows ? 2 : 1) + 1;
        const std::size_t limit_cells = settings.memory_limit_bytes / sizeof(Real);
        if (N > limit_cells / n / matrices)
            throw InvalidInput(
                "exposure engine: " + std::to_string(K) + " trades x " + std::to_string(N) +
                " paths x " + std::to_string(n) + " dates exceed the memory limit of " +
                std::to_string(settings.memory_limit_bytes >> 20) +
                " MiB; reduce the paths or raise memory_limit_bytes");

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
        std::vector<Real> q_decay(n), q_drift(n), q_sd(n), q_scale(n), q_G(n);
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
        std::vector<Real> decay = q_decay, drift = q_drift, sd = q_sd, weight_scale = q_scale,
                          weight_G = q_G;
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

        out.discount_weight.resize(N * n);
        out.trade_values.assign(K, std::vector<Real>(N * n));
        if (settings.keep_cashflows)
            out.trade_cashflows.assign(K, std::vector<Real>(N * n));

        const auto run_chunk = [&](std::size_t first, std::size_t last)
        {
            std::vector<Real> state(n), values(n), flows(n);
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

} // namespace quantModeling
