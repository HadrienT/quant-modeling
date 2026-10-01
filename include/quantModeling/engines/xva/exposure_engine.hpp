#ifndef QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP
#define QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP

#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/market/historical_rate_dynamics.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace quantModeling
{

    /// Density of the exposure grid (blueprint/wp/23-xva.md §3.4).
    struct ExposureGridSettings
    {
        /// Weekly steps up to this date: where the margin period of risk and
        /// the one-year EEPE are decided.
        Time weekly_until = 1.0 / 12.0;
        /// Monthly steps up to this date, quarterly beyond.
        Time monthly_until = 2.0;
        /// When > 0, every date t of the grid brings its lagged date
        /// t - MPoR with it: collateralised exposure needs the value at both
        /// (risk/collateral.hpp). Set it to the CSA's margin period of risk.
        Time margin_period_of_risk = 0.0;
    };

    /**
     * @brief The dates at which exposure is measured: a regular grid that
     *        thins out with time, plus every event date of the trades
     *        (fixings, payments, exercise) up to the horizon.
     *
     * The value of a swap jumps at each payment; a grid that skips the
     * payment dates misses those jumps.
     *
     * @return t_1 < ... < t_n = horizon, without today.
     * @throws InvalidInput unless horizon > 0.
     */
    std::vector<Time> exposure_grid(Time horizon, const std::vector<Time> &event_times,
                                    const ExposureGridSettings &settings = {});

    struct ExposureSimulationSettings
    {
        std::size_t paths = 10000;
        std::uint64_t seed = 42;
        /// Also store the cash flows of each trade (they double the memory;
        /// the collateral model of lot X2 needs them).
        bool keep_cashflows = false;
        ExposureGridSettings grid;
        /// The cube takes trades × paths × dates × 8 bytes (twice that with
        /// cash flows). Above this limit simulate() refuses to run instead of
        /// exhausting a machine whose memory is shared.
        std::size_t memory_limit_bytes = std::size_t{4} << 30;
        /**
         * @brief When set, the scenarios are drawn under the **historical**
         *        measure: the short rate follows these dynamics, estimated
         *        on its past (market/historical_rate_dynamics.hpp), from
         *        today's short rate.
         *
         * Each trade is still priced on each path by the risk-neutral model
         * — what it would be worth in that scenario is a market price. Only
         * the probability of the scenarios changes. The cube is then for
         * risk measures (PFE, EPE, EEPE), not for xVA.
         */
        std::optional<HistoricalRateDynamics> historical;
    };

    /**
     * @brief Exposure engine under one-factor Hull-White
     *        (blueprint/wp/23-xva.md §13.3, lot X1).
     *
     * The algorithm of Gregory's exposure chapter (and Pykhtin & Zhu 2007):
     * simulate the risk factor on a grid of dates, reprice every trade at
     * every date on every path. Here the factor is the Hull-White state
     * x(t) = r(t) - f(0, t), and repricing is exact (ADR-X1).
     *
     * **Measure.** x is simulated with its exact Gaussian transitions under
     * the T*-forward measure, T* the last date of the grid: the numeraire
     * P(t, T*) is a closed form of x(t), so the discounted exposure
     *
     *   EE*(t) = E^Q[D(t) max(V(t), 0)] = P(0, T*) E^{T*}[max(V(t), 0) / P(t, T*)]
     *
     * needs no simulated bank account and has no time-discretisation error.
     *
     * With `ExposureSimulationSettings::historical` the same state is driven
     * by the real-world dynamics of the short rate instead, x(t) = r(t) -
     * f(0, t), and nothing is discounted: scenarios for risk measures.
     *
     * **Reproducibility.** Draw j of path p is a pure function of
     * (seed, p, j) (Philox, as the GPU engines): the cube is the same, bit
     * for bit, on one thread or on fifty.
     *
     * The engine does not know what a swap is: it only sees FutureValue.
     * The model must outlive the engine.
     */
    class HullWhiteExposureEngine
    {
      public:
        explicit HullWhiteExposureEngine(const HullWhiteCurveModel &model);

        /// Adds a trade; a negative quantity is the opposite position (a
        /// sold swaption). Returns the index of the trade in the cube.
        /// @throws UnsupportedInstrument if the instrument has no future
        ///         value under this model.
        std::size_t add(const Instrument &instrument, Real quantity = 1.0);
        std::size_t add(std::unique_ptr<FutureValue> trade, Real quantity = 1.0);

        std::size_t trades() const { return trades_.size(); }

        /// The grid simulate() will use: exposure_grid() over the longest
        /// trade, with the event dates of all of them.
        std::vector<Time> grid(const ExposureGridSettings &settings = {}) const;

        /**
         * @brief Runs the simulation.
         * @param pool worker threads; nullptr runs on the calling thread.
         * @throws InvalidInput without any trade, with zero paths, or when
         *         the cube would exceed settings.memory_limit_bytes.
         */
        ExposurePaths simulate(const ExposureSimulationSettings &settings = {},
                               ThreadPool *pool = nullptr);

      private:
        struct Trade
        {
            std::unique_ptr<FutureValue> value;
            Real quantity;
        };

        const HullWhiteCurveModel &model_;
        std::vector<Trade> trades_;
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP
