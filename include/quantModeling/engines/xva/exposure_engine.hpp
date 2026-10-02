#ifndef QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP
#define QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP

#include "quantModeling/engines/xva/exposure_program.hpp"
#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/market/historical_rate_dynamics.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
        /**
         * @brief Paths of the pilot simulation the regression-based trades
         *        are fitted on (lot X4); 0 picks four times the main paths,
         *        between 20 000 and 200 000. Ignored when every trade has a
         *        closed form.
         *
         * More than the main simulation, on purpose: a pilot path only
         * evaluates cash flows, and the noise of the fitted values — the
         * future cash flows of years are regressed on one number — is what
         * limits the accuracy of the exposure, not the bias of the basis.
         *
         * The pilot is independent of the main simulation (another seed)
         * and always drawn under the pricing measure. Under the historical
         * measure its paths start from a dispersed state, wide enough for
         * the fitted values to cover where the historical scenarios go:
         * a regression says nothing outside the range it was fitted on.
         */
        std::size_t pilot_paths = 0;
        /**
         * @brief Also compute ISDA SIMM (risk/simm.hpp) of the trades taken
         *        together, on every path at every date: each trade gives its
         *        sensitivities in that scenario, in closed form.
         *
         * Swaps and European swaptions only: a trade valued by regression
         * (a Bermudan, a script) knows its value as a function of the
         * model's state, not its sensitivity to each point of the curve, and
         * simulate() refuses it.
         */
        bool simm = false;
        /// The currency's SIMM parameters; USD, EUR and GBP by default.
        simm::CurrencyParameters simm_currency = simm::regular_well_traded();
        /// Standard deviation of the state the pilot paths start from;
        /// unset is automatic: 0 under the pricing measure, what covers the
        /// scenarios under the historical one.
        std::optional<Real> pilot_dispersion;
        /**
         * @brief Where the paths are valued (blueprint/wp/23-xva.md §14.11,
         *        lot X7). On the GPU every trade must have a closed form
         *        (FutureValue::compile): a trade valued by regression, and
         *        SIMM on every path, stay on the CPU.
         *
         * Auto takes the GPU when a device is present and the simulation can
         * run there, the CPU otherwise, and says why in the result's
         * `device_note`. Gpu refuses to fall back.
         *
         * The scenarios are the CPU's (the same Philox draws); the values
         * agree to the last bits of exp and erfc, not bit for bit. The result
         * is the same bits on one card or two.
         */
        ComputeDevice device = ComputeDevice::Cpu;
        /// The cards a GPU run shares its paths between: the first
        /// `max_gpus` usable ones, all of them for 0.
        int max_gpus = 0;
        /// Logical blocks (4 096 paths) per GPU launch; 0 takes what half of
        /// the free device memory holds. The result does not depend on it.
        std::uint64_t gpu_blocks_per_launch = 0;
    };

    /// A netting set whose exposure is wanted without its cube
    /// (HullWhiteExposureEngine::simulate_netting_set).
    struct NettingSetRequest
    {
        /// The netted trades, as indices into the engine's; empty means all.
        std::vector<std::size_t> trades;
        /// The CSA, or none for an uncollateralised netting set. Its margin
        /// period of risk is put on the grid.
        std::optional<Csa> csa;
        /// The treatment of the flows of the margin period, and initial
        /// margin as profiles over the reporting dates. A margin that depends
        /// on the path needs the cube: simulate(), then collateralise().
        CollateralSettings collateral;
        /**
         * @brief Optional weights a_r and b_r of the per-path sums
         *
         *   Σ_r a_r D max(V, 0)   and   Σ_r b_r D min(V, 0)
         *
         * over the reporting dates, which the function is called with:
         * it fills `on_positive` and `on_negative`, one value per date. With
         * a_r = -LGD × the default probability of (t_{r-1}, t_r] the first
         * sum is the CVA of the path; its mean is CVA and its dispersion the
         * Monte-Carlo error of CVA, correlations between dates included.
         */
        std::function<void(const std::vector<Time> &times, std::vector<Real> &on_positive,
                           std::vector<Real> &on_negative)>
            weights;
    };

    /// The exposure of a netting set, reduced over the paths.
    struct NettingSetExposure
    {
        /// On the reporting dates. Reduced on a device it has no PFE and no
        /// Euler allocation: both need the paths.
        ExposureStatistics statistics;
        /// The two weighted sums of NettingSetRequest::weights.
        Estimate weighted_positive, weighted_negative;
        std::size_t paths = 0;
        /// "cpu" or "gpu", how many cards, and why the CPU when the request
        /// left the choice.
        std::string device = "cpu";
        int gpus = 0;
        std::string device_note;
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

        /**
         * @brief Exposure of one netting set, collateralised or not, without
         *        keeping the cube: the trades are netted, the CSA applied and
         *        the profiles reduced path by path.
         *
         * On the GPU everything happens on the device and only the profiles
         * come back: the number of paths is bounded by neither the host's
         * memory nor the copy. On the CPU it is simulate(), collateralise()
         * and exposure_statistics() in a row — the reference the device is
         * tested against.
         *
         * @throws InvalidInput on an initial margin without a CSA or given
         *         per path, on a trade index out of range or repeated.
         */
        NettingSetExposure simulate_netting_set(ExposureSimulationSettings settings,
                                                const NettingSetRequest &request,
                                                ThreadPool *pool = nullptr);

        /**
         * @brief CVA, DVA, FCA and FBA of the netting set of all the trades,
         *        and their sensitivities to every input, by adjoint
         *        differentiation (engines/xva/xva_risks.hpp, lot X8).
         *
         * `settings` gives the paths, the seed and the grid; the CSA's margin
         * period of risk is put on the grid.
         *
         * @throws InvalidInput when a trade is valued by regression, when the
         *         model has two curves, or under the historical measure.
         */
        XvaRisks xva_risks(ExposureSimulationSettings settings, const XvaRiskInputs &inputs,
                           ThreadPool *pool = nullptr);

        /// The same estimator in plain doubles, without sensitivities: what
        /// xva_risks() is checked against by finite differences, and what its
        /// cost is measured against.
        XvaValues xva_values(ExposureSimulationSettings settings, const XvaRiskInputs &inputs,
                             ThreadPool *pool = nullptr);

        /// Standard deviation of the pilot's starting state in the last
        /// simulate(): 0 unless the main simulation ran under the historical
        /// measure with a trade valued by regression.
        Real pilot_dispersion() const { return pilot_dispersion_; }

      private:
        struct Trade
        {
            std::unique_ptr<FutureValue> value;
            Real quantity;
        };

        /// How the state moves on the grid: under the simulation's measure,
        /// and under the pricing measure (the pilot's, always).
        struct Dynamics
        {
            xva::StateDynamics main;
            xva::StateDynamics pricing;
        };

        /// The grid, the trades bound to it, and the dynamics: fills the
        /// dates, the measure and the discount factors of `out`.
        Dynamics prepare(const ExposureSimulationSettings &settings, ExposurePaths &out);

        /// True when the run goes to the device, with the trades compiled in
        /// `program`. Otherwise `note` says why — or, for a run that insisted
        /// on the GPU, the reason is thrown.
        bool on_device(const ExposureSimulationSettings &settings, xva::ExposureProgram &program,
                       std::string &note) const;

        /// What a run of xva_risks() or xva_values() is set up with
        /// (src/engines/xva/xva_risks.cpp).
        struct RiskRun;
        void prepare_risk_run(ExposureSimulationSettings &settings, const XvaRiskInputs &inputs,
                              RiskRun &run);

        const HullWhiteCurveModel &model_;
        std::vector<Trade> trades_;
        Real pilot_dispersion_ = 0.0;
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_EXPOSURE_ENGINE_HPP
