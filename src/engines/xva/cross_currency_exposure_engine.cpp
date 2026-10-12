#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"

#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/utils/stats.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;
        /// Paths per task, as in the one-currency engine.
        constexpr std::size_t kChunk = 256;
        /// The streams of the foreign rate and of the exchange rate: the
        /// domestic rate keeps the seed itself, the one-currency engine's.
        constexpr std::uint64_t kForeignStream = 0x9E3779B97F4A7C15ULL;
        constexpr std::uint64_t kFxStream = 0xC2B2AE3D27D4EB4FULL;

        /// A cash flow known today: > 0 received by the bank.
        struct Flow
        {
            Time time;
            Real amount;
        };

        /// A stream of known flows in one currency, valued on the grid:
        /// Σ_{T_j > t_i} c_j P(t_i, T_j | x), each bond a closed form of x.
        class Leg
        {
          public:
            Leg(std::vector<Flow> flows, const HullWhiteCurveModel &model)
                : flows_(std::move(flows)), model_(model)
            {
                std::sort(flows_.begin(), flows_.end(),
                          [](const Flow &a, const Flow &b)
                          { return a.time < b.time; });
            }

            const std::vector<Flow> &flows() const { return flows_; }

            Real value_today() const
            {
                Real sum = 0.0;
                for (const Flow &f : flows_)
                    sum += f.amount * model_.discount().discount(f.time);
                return sum;
            }

            void bind(const std::vector<Time> &grid)
            {
                const std::size_t n = grid.size();
                first_.assign(n + 1, 0);
                coefficient_.clear();
                G_.clear();
                paid_.assign(n, 0.0);
                for (const Flow &f : flows_)
                {
                    const auto at = std::lower_bound(grid.begin(), grid.end(), f.time - kTimeEps);
                    if (at == grid.end() || std::abs(*at - f.time) > kTimeEps)
                        throw InvalidInput("two-currency engine: a payment date is not on the grid");
                    paid_[static_cast<std::size_t>(at - grid.begin())] += f.amount;
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Time t = grid[i];
                    first_[i] = coefficient_.size();
                    const Real y = model_.y(t), today = model_.discount().discount(t);
                    for (const Flow &f : flows_)
                    {
                        if (f.time <= t + kTimeEps)
                            continue;
                        const Real g = model_.G(t, f.time);
                        // c P(0, T) / P(0, t) exp(-½ G² y(t)), then exp(-G x).
                        coefficient_.push_back(f.amount * model_.discount().discount(f.time) / today *
                                               std::exp(-0.5 * g * g * y));
                        G_.push_back(g);
                    }
                }
                first_[n] = coefficient_.size();
            }

            Real value(std::size_t i, Real x) const
            {
                Real sum = 0.0;
                for (std::size_t j = first_[i]; j < first_[i + 1]; ++j)
                    sum += coefficient_[j] * std::exp(-G_[j] * x);
                return sum;
            }

            Real paid(std::size_t i) const { return paid_[i]; }

          private:
            std::vector<Flow> flows_;
            const HullWhiteCurveModel &model_;
            std::vector<std::size_t> first_;
            std::vector<Real> coefficient_, G_, paid_;
        };

        class CashflowStreams final : public CrossCurrencyFutureValue
        {
          public:
            CashflowStreams(std::vector<Flow> domestic, std::vector<Flow> foreign,
                            const CrossCurrencyHullWhiteModel &model)
                : domestic_(std::move(domestic), model.domestic()),
                  foreign_(std::move(foreign), model.foreign()),
                  spot_(model.spot())
            {
                for (const Leg *leg : {&domestic_, &foreign_})
                    for (const Flow &f : leg->flows())
                        if (!(f.time > kTimeEps) || !std::isfinite(f.time) || !std::isfinite(f.amount))
                            throw InvalidInput("two-currency engine: cash flows must be finite and in "
                                               "the future");
                if (domestic_.flows().empty() && foreign_.flows().empty())
                    throw InvalidInput("two-currency engine: a trade without cash flows");
            }

            std::vector<Time> event_times() const override
            {
                std::vector<Time> times;
                for (const Leg *leg : {&domestic_, &foreign_})
                    for (const Flow &f : leg->flows())
                        times.push_back(f.time);
                return times;
            }

            Time maturity() const override
            {
                Time last = 0.0;
                for (const Leg *leg : {&domestic_, &foreign_})
                    if (!leg->flows().empty())
                        last = std::max(last, leg->flows().back().time);
                return last;
            }

            Real value_today() const override
            {
                return domestic_.value_today() + spot_ * foreign_.value_today();
            }

            void bind(const std::vector<Time> &grid) override
            {
                domestic_.bind(grid);
                foreign_.bind(grid);
            }

            void evaluate_path(const CrossCurrencyPath &path, std::size_t dates, Real *values,
                               Real *cashflows) const override
            {
                for (std::size_t i = 0; i < dates; ++i)
                    values[i] = domestic_.value(i, path.domestic[i]) +
                                path.spot[i] * foreign_.value(i, path.foreign[i]);
                if (cashflows != nullptr)
                    for (std::size_t i = 0; i < dates; ++i)
                        cashflows[i] = domestic_.paid(i) + path.spot[i] * foreign_.paid(i);
            }

            bool describe(xva::TwoFactorTerms &terms) const override
            {
                for (const Flow &f : domestic_.flows())
                    terms.domestic.push_back({f.time, f.amount});
                for (const Flow &f : foreign_.flows())
                    terms.foreign.push_back({f.time, f.amount});
                return true;
            }

          private:
            Leg domestic_, foreign_;
            Real spot_;
        };

        /// Undiscounted Black price on a forward, from the standard
        /// deviation of its logarithm at the expiry.
        Real black(Real forward, Real strike, Real deviation, bool call)
        {
            if (!(deviation > 1e-12))
                return std::max(call ? forward - strike : strike - forward, 0.0);
            const Real d1 = std::log(forward / strike) / deviation + 0.5 * deviation;
            const Real d2 = d1 - deviation;
            // Far out of the money the difference of two rounded products
            // can fall a hair below zero: a price does not.
            return std::max(call ? forward * norm_cdf(d1) - strike * norm_cdf(d2)
                                 : strike * norm_cdf(-d2) - forward * norm_cdf(-d1),
                            0.0);
        }

        /**
         * A European option on the exchange rate, settled in cash in
         * domestic currency, **bought** — an option on an asset when the
         * "foreign currency" is one (make_asset_model). At t < T,
         *
         *   V(t) = N P_d(t, T) Black(F(t, T), K, √v(t, T)),
         *   F(t, T) = S(t) P_f(t, T) / P_d(t, T),
         *
         * v(t, T) the variance the forward still has to accumulate, known
         * today (CrossCurrencyHullWhiteModel::forward_variance): a closed
         * form of the state on every path. At T it pays N (S − K)^+ or
         * N (K − S)^+, and is worth nothing after.
         */
        class SpotOption final : public CrossCurrencyFutureValue
        {
          public:
            SpotOption(Real strike, Time expiry, bool call, Real notional,
                       const CrossCurrencyHullWhiteModel &model)
                : strike_(strike), expiry_(expiry), call_(call), notional_(notional), model_(model)
            {
                if (!(strike_ > 0.0) || !std::isfinite(strike_))
                    throw InvalidInput("spot option: the strike must be finite and > 0");
                if (!(expiry_ > kTimeEps) || !std::isfinite(expiry_))
                    throw InvalidInput("spot option: the expiry must be in the future");
                if (!std::isfinite(notional_))
                    throw InvalidInput("spot option: the notional must be finite");
            }

            std::vector<Time> event_times() const override { return {expiry_}; }
            Time maturity() const override { return expiry_; }

            Real value_today() const override
            {
                return notional_ * model_.domestic().discount().discount(expiry_) *
                       black(model_.forward_fx(expiry_), strike_,
                             std::sqrt(model_.forward_variance(0.0, expiry_)), call_);
            }

            void bind(const std::vector<Time> &grid) override
            {
                const auto at = std::lower_bound(grid.begin(), grid.end(), expiry_ - kTimeEps);
                if (at == grid.end() || std::abs(*at - expiry_) > kTimeEps)
                    throw InvalidInput("two-currency engine: an expiry is not on the grid");
                expiry_index_ = static_cast<std::size_t>(at - grid.begin());
                times_.assign(grid.begin(), grid.begin() + static_cast<std::ptrdiff_t>(expiry_index_));
                deviation_.clear();
                for (const Time t : times_)
                    deviation_.push_back(std::sqrt(model_.forward_variance(t, expiry_)));
            }

            void evaluate_path(const CrossCurrencyPath &path, std::size_t dates, Real *values,
                               Real *cashflows) const override
            {
                const std::size_t alive = std::min(dates, expiry_index_);
                for (std::size_t i = 0; i < alive; ++i)
                {
                    const Real domestic = model_.domestic().zcb(times_[i], expiry_, path.domestic[i]);
                    const Real foreign = model_.foreign().zcb(times_[i], expiry_, path.foreign[i]);
                    values[i] = notional_ * domestic *
                                black(path.spot[i] * foreign / domestic, strike_, deviation_[i], call_);
                }
                for (std::size_t i = alive; i < dates; ++i)
                    values[i] = 0.0;
                if (cashflows == nullptr)
                    return;
                for (std::size_t i = 0; i < dates; ++i)
                    cashflows[i] = 0.0;
                if (expiry_index_ < dates)
                {
                    const Real spot = path.spot[expiry_index_];
                    cashflows[expiry_index_] =
                        notional_ * std::max(call_ ? spot - strike_ : strike_ - spot, 0.0);
                }
            }

            bool describe(xva::TwoFactorTerms &terms) const override
            {
                terms.options.push_back({strike_, expiry_, call_, notional_});
                return true;
            }

          private:
            Real strike_;
            Time expiry_;
            bool call_;
            Real notional_;
            const CrossCurrencyHullWhiteModel &model_;
            std::size_t expiry_index_ = 0;
            std::vector<Time> times_;
            std::vector<Real> deviation_;
        };

        /// The instrument as its two streams of flows — or, for an option,
        /// as its own future value.
        class StreamBuilder final : public IInstrumentVisitor
        {
          public:
            explicit StreamBuilder(const CrossCurrencyHullWhiteModel &model)
                : model_(model) {}

            std::vector<Flow> domestic, foreign;
            std::unique_ptr<CrossCurrencyFutureValue> option;

            /// A European option on the exchange rate (or on the asset the
            /// foreign currency stands for), cash-settled, on `notional`
            /// units.
            void visit(const VanillaOption &o) override
            {
                if (!o.payoff || !o.exercise || o.exercise->type() != ExerciseType::European ||
                    o.exercise->dates().size() != 1)
                    throw UnsupportedInstrument("The two-currency exposure engine values European "
                                                "options only.");
                option = std::make_unique<SpotOption>(o.payoff->strike(), o.exercise->dates().front(),
                                                      o.payoff->type() == OptionType::Call, o.notional,
                                                      model_);
            }

            void visit(const FXForward &forward) override
            {
                if (!(forward.maturity > 0.0) || !(forward.strike > 0.0))
                    throw InvalidInput("FX forward: maturity and delivery rate must be > 0");
                // Receives the foreign notional, pays it at the delivery rate.
                foreign.push_back({forward.maturity, forward.notional});
                domestic.push_back({forward.maturity, -forward.notional * forward.strike});
            }

            void visit(const CrossCurrencySwap &swap) override
            {
                if (!(swap.maturity > 0.0) || swap.frequency < 1)
                    throw InvalidInput("cross-currency swap: maturity must be > 0 and frequency >= 1");
                if (!(swap.foreign_notional > 0.0) || !(swap.domestic_notional > 0.0))
                    throw InvalidInput("cross-currency swap: both notionals must be > 0");
                const Real coupons = swap.maturity * swap.frequency;
                const long count = std::lround(coupons);
                if (count < 1 || std::abs(coupons - static_cast<Real>(count)) > 1e-8)
                    throw InvalidInput("cross-currency swap: the maturity must be a whole number of "
                                       "coupon periods");
                const Real side = swap.receive_foreign ? 1.0 : -1.0;
                const Real period = 1.0 / swap.frequency;
                for (long k = 1; k <= count; ++k)
                {
                    const Time t = k == count ? swap.maturity : static_cast<Real>(k) * period;
                    foreign.push_back({t, side * swap.foreign_notional * swap.foreign_rate * period});
                    domestic.push_back({t, -side * swap.domestic_notional * swap.domestic_rate * period});
                }
                // The notionals come back at maturity.
                foreign.push_back({swap.maturity, side * swap.foreign_notional});
                domestic.push_back({swap.maturity, -side * swap.domestic_notional});
            }

            // The visitor's pure virtuals: path-dependent equity products and
            // bonds have no closed form here.
            void visit(const AsianOption &) override { unsupported(); }
            void visit(const BarrierOption &) override { unsupported(); }
            void visit(const DigitalOption &) override { unsupported(); }
            void visit(const EquityFuture &) override { unsupported(); }
            void visit(const ZeroCouponBond &) override { unsupported(); }
            void visit(const FixedRateBond &) override { unsupported(); }

          private:
            [[noreturn]] static void unsupported()
            {
                throw UnsupportedInstrument("This instrument has no future value in the two-currency "
                                            "exposure engine.");
            }

            const CrossCurrencyHullWhiteModel &model_;
        };

        /// A trade of one currency: the one-currency future value, read on
        /// that currency's rate, converted at the spot when it is foreign.
        class SingleCurrency final : public CrossCurrencyFutureValue
        {
          public:
            SingleCurrency(std::unique_ptr<FutureValue> trade, bool foreign, Real spot_today)
                : trade_(std::move(trade)), foreign_(foreign), spot_today_(spot_today)
            {
                if (!trade_)
                    throw InvalidInput("two-currency engine: null trade");
                if (trade_->needs_pilot())
                    throw UnsupportedInstrument("A trade valued by regression is not supported by the "
                                                "two-currency exposure engine.");
            }

            std::vector<Time> event_times() const override { return trade_->event_times(); }
            Time maturity() const override { return trade_->maturity(); }
            Real value_today() const override
            {
                return (foreign_ ? spot_today_ : 1.0) * trade_->value_today();
            }
            void bind(const std::vector<Time> &grid) override { trade_->bind(grid); }

            void evaluate_path(const CrossCurrencyPath &path, std::size_t dates, Real *values,
                               Real *cashflows) const override
            {
                trade_->evaluate_path(foreign_ ? path.foreign : path.domestic, dates, values, cashflows,
                                      nullptr);
                if (!foreign_)
                    return;
                for (std::size_t i = 0; i < dates; ++i)
                    values[i] *= path.spot[i];
                if (cashflows != nullptr)
                    for (std::size_t i = 0; i < dates; ++i)
                        cashflows[i] *= path.spot[i];
            }

          private:
            std::unique_ptr<FutureValue> trade_;
            bool foreign_;
            Real spot_today_;
        };
    } // namespace

    std::unique_ptr<CrossCurrencyFutureValue>
    make_cross_currency_future_value(const Instrument &instrument,
                                     const CrossCurrencyHullWhiteModel &model)
    {
        StreamBuilder builder(model);
        instrument.accept(builder);
        if (builder.option)
            return std::move(builder.option);
        return std::make_unique<CashflowStreams>(std::move(builder.domestic),
                                                 std::move(builder.foreign), model);
    }

    std::unique_ptr<CrossCurrencyFutureValue> in_domestic_currency(std::unique_ptr<FutureValue> trade)
    {
        return std::make_unique<SingleCurrency>(std::move(trade), false, 1.0);
    }

    std::unique_ptr<CrossCurrencyFutureValue> in_foreign_currency(std::unique_ptr<FutureValue> trade,
                                                                  Real spot_today)
    {
        return std::make_unique<SingleCurrency>(std::move(trade), true, spot_today);
    }

    CrossCurrencyExposureEngine::CrossCurrencyExposureEngine(const CrossCurrencyHullWhiteModel &model)
        : model_(model)
    {
    }

    std::size_t CrossCurrencyExposureEngine::add(const Instrument &instrument, Real quantity)
    {
        return add(make_cross_currency_future_value(instrument, model_), quantity);
    }

    std::size_t CrossCurrencyExposureEngine::add_domestic(const Instrument &instrument, Real quantity)
    {
        return add(in_domestic_currency(make_hull_white_future_value(instrument, model_.domestic())),
                   quantity);
    }

    std::size_t CrossCurrencyExposureEngine::add_foreign(const Instrument &instrument, Real quantity)
    {
        return add(in_foreign_currency(make_hull_white_future_value(instrument, model_.foreign()),
                                       model_.spot()),
                   quantity);
    }

    std::size_t CrossCurrencyExposureEngine::add(std::unique_ptr<CrossCurrencyFutureValue> trade,
                                                 Real quantity)
    {
        if (!trade)
            throw InvalidInput("two-currency engine: null trade");
        if (!std::isfinite(quantity))
            throw InvalidInput("two-currency engine: the quantity must be finite");
        trades_.push_back({std::move(trade), quantity});
        return trades_.size() - 1;
    }

    std::vector<Time> CrossCurrencyExposureEngine::grid(const ExposureGridSettings &settings) const
    {
        if (trades_.empty())
            throw InvalidInput("two-currency engine: no trade");
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

    ExposurePaths CrossCurrencyExposureEngine::simulate(const ExposureSimulationSettings &settings,
                                                        ThreadPool *pool,
                                                        CrossCurrencyScenarios *scenarios)
    {
        if (settings.paths == 0)
            throw InvalidInput("two-currency engine: the number of paths must be > 0");
        if (settings.historical)
            throw InvalidInput("two-currency engine: scenarios under the historical measure are not "
                               "supported");
        if (settings.simm)
            throw InvalidInput("two-currency engine: SIMM on every path is not supported");
        if (settings.device == ComputeDevice::Gpu)
            throw InvalidInput("two-currency engine: it runs on the CPU only");

        ExposurePaths out;
        out.times = grid(settings.grid);
        if (settings.device == ComputeDevice::Auto)
            out.device_note = "the two-currency exposure engine runs on the CPU";
        const std::size_t n = out.times.size(), N = settings.paths, K = trades_.size();
        const Time horizon = out.times.back();

        const std::size_t matrices = K * (settings.keep_cashflows ? 2 : 1) + 1 + (scenarios ? 3 : 0);
        if (N > settings.memory_limit_bytes / sizeof(Real) / n / matrices)
            throw InvalidInput("two-currency engine: " + std::to_string(K) + " trades x " +
                               std::to_string(N) + " paths x " + std::to_string(n) +
                               " dates exceed the memory limit of " +
                               std::to_string(settings.memory_limit_bytes >> 20) +
                               " MiB; reduce the paths or raise memory_limit_bytes");

        for (Trade &trade : trades_)
        {
            trade.value->bind(out.times);
            out.trade_values_today.push_back(trade.quantity * trade.value->value_today());
        }

        // The transitions, the discount weight P_d(0, T*) / P_d(t, T*) and
        // the spot F(0, T*) e^z P_d(t, T*) / P_f(t, T*), date by date.
        const HullWhiteCurveModel &domestic = model_.domestic(), &foreign = model_.foreign();
        std::vector<CrossCurrencyHullWhiteModel::Transition> steps(n);
        std::vector<Real> weight_scale(n), G_domestic(n), G_foreign(n), spot_scale(n);
        const Real forward = model_.forward_fx(horizon);
        Time previous = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Time t = out.times[i];
            steps[i] = model_.transition(previous, t, horizon);
            G_domestic[i] = domestic.G(t, horizon);
            G_foreign[i] = foreign.G(t, horizon);
            const Real convexity_d = 0.5 * G_domestic[i] * G_domestic[i] * domestic.y(t);
            const Real convexity_f = 0.5 * G_foreign[i] * G_foreign[i] * foreign.y(t);
            const Real today_d = domestic.discount().discount(t), today_f = foreign.discount().discount(t);
            weight_scale[i] = today_d * std::exp(convexity_d);
            spot_scale[i] = forward * (domestic.discount().discount(horizon) / today_d) /
                            (foreign.discount().discount(horizon) / today_f) *
                            std::exp(convexity_f - convexity_d);
            out.discount.push_back(today_d);
            previous = t;
        }

        out.paths = N;
        out.discount_weight.resize(N * n);
        out.trade_values.assign(K, std::vector<Real>(N * n));
        if (settings.keep_cashflows)
            out.trade_cashflows.assign(K, std::vector<Real>(N * n));
        if (scenarios != nullptr)
            for (std::vector<Real> *matrix : {&scenarios->domestic, &scenarios->foreign, &scenarios->spot})
                matrix->assign(N * n, 0.0);

        const auto run_chunk = [&](std::size_t first, std::size_t last)
        {
            std::vector<Real> x_d(n), x_f(n), spot(n), values(n), flows(n);
            PhiloxGaussianSource draw_d(settings.seed), draw_f(settings.seed ^ kForeignStream),
                draw_s(settings.seed ^ kFxStream);
            for (std::size_t p = first; p < last; ++p)
            {
                draw_d.set_path(p);
                draw_f.set_path(p);
                draw_s.set_path(p);
                Real d = 0.0, f = 0.0, z = 0.0;
                const std::size_t row = p * n;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const CrossCurrencyHullWhiteModel::Transition &step = steps[i];
                    const Real z0 = draw_d.next(), z1 = draw_f.next(), z2 = draw_s.next();
                    d = step.decay[0] * d + step.drift[0] + step.cholesky[0][0] * z0;
                    f = step.decay[1] * f + step.drift[1] + step.cholesky[1][0] * z0 +
                        step.cholesky[1][1] * z1;
                    z += step.drift[2] + step.cholesky[2][0] * z0 + step.cholesky[2][1] * z1 +
                         step.cholesky[2][2] * z2;
                    x_d[i] = d;
                    x_f[i] = f;
                    spot[i] = spot_scale[i] * std::exp(z - G_domestic[i] * d + G_foreign[i] * f);
                    out.discount_weight[row + i] = weight_scale[i] * std::exp(G_domestic[i] * d);
                }
                if (scenarios != nullptr)
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        scenarios->domestic[row + i] = x_d[i];
                        scenarios->foreign[row + i] = x_f[i];
                        scenarios->spot[row + i] = spot[i];
                    }
                const CrossCurrencyPath path{x_d.data(), x_f.data(), spot.data()};
                for (std::size_t k = 0; k < K; ++k)
                {
                    const Real quantity = trades_[k].quantity;
                    trades_[k].value->evaluate_path(path, n, values.data(),
                                                    settings.keep_cashflows ? flows.data() : nullptr);
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
