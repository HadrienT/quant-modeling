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

        std::sort(times.begin(), times.end());
        std::vector<Time> grid;
        for (const Time t : times)
            if (grid.empty() || t - grid.back() > kTimeEps)
                grid.push_back(t);
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
        {
            trade.value->bind(out.times);
            out.trade_values_today.push_back(trade.quantity * trade.value->value_today());
        }

        // Exact transitions of x under the T*-forward measure, and the
        // discount weight P(0, T*) / P(t, T* | x) = P(0, t) exp(G x + G² y / 2),
        // G = G(t, T*).
        const Time horizon = out.times.back();
        std::vector<Real> decay(n), drift(n), sd(n), weight_scale(n), weight_G(n);
        Time previous = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Time t = out.times[i];
            const auto tr = model_.transition(previous, t, horizon);
            decay[i] = tr.decay;
            drift[i] = tr.drift;
            sd[i] = std::sqrt(tr.variance);
            const Real G = model_.G(t, horizon);
            out.discount.push_back(model_.discount().discount(t));
            weight_G[i] = G;
            weight_scale[i] = out.discount[i] * std::exp(0.5 * G * G * model_.y(t));
            previous = t;
        }

        out.discount_weight.resize(N * n);
        out.trade_values.assign(K, std::vector<Real>(N * n));
        if (settings.keep_cashflows)
            out.trade_cashflows.assign(K, std::vector<Real>(N * n));

        const auto run_chunk = [&](std::size_t first, std::size_t last)
        {
            std::vector<Real> state(n);
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
                    const FutureValue &trade = *trades_[k].value;
                    const Real quantity = trades_[k].quantity;
                    std::vector<Real> &values = out.trade_values[k];
                    for (std::size_t i = 0; i < n; ++i)
                        values[row + i] = quantity * trade.value(i, state.data());
                    if (settings.keep_cashflows)
                    {
                        std::vector<Real> &flows = out.trade_cashflows[k];
                        for (std::size_t i = 0; i < n; ++i)
                            flows[row + i] = quantity * trade.cashflow(i, state.data());
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
