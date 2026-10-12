#ifndef QM_ENGINES_XVA_CROSS_CURRENCY_EXPOSURE_ENGINE_HPP
#define QM_ENGINES_XVA_CROSS_CURRENCY_EXPOSURE_ENGINE_HPP

#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/models/rates/cross_currency_hull_white.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <memory>
#include <vector>

namespace quantModeling
{

    namespace xva
    {
        /**
         * @brief A trade of the two-currency engine in flat terms: the cash
         *        flows known today in each currency and the European options
         *        on the exchange rate it is made of, for one unit of it. What
         *        the adjoint run differentiates (§14.17): it replays the
         *        valuation on a differentiable number, which a virtual
         *        `evaluate_path` on doubles cannot do.
         */
        struct TwoFactorTerms
        {
            struct Flow
            {
                Time time;
                Real amount; ///< > 0 received by the bank
            };
            struct Option
            {
                Real strike;
                Time expiry;
                bool call;
                Real notional; ///< in units of the foreign currency (or of the asset)
            };
            std::vector<Flow> domestic, foreign;
            std::vector<Option> options;
        };
    } // namespace xva

    /// One path of the model on the exposure grid: the state of each rate
    /// and the spot (domestic per foreign), date by date.
    struct CrossCurrencyPath
    {
        const Real *domestic;
        const Real *foreign;
        const Real *spot;
    };

    /**
     * @brief What a trade is worth, in **domestic currency**, at each date
     *        of one path of the two-currency model: the seam of
     *        engines/xva/future_value.hpp with three factors instead of one
     *        (blueprint/wp/23-xva.md §14.15, lot X10).
     *
     * After bind() the object is const and touches no shared state: paths
     * are valued from several threads at once.
     */
    class CrossCurrencyFutureValue
    {
      public:
        virtual ~CrossCurrencyFutureValue() = default;

        /// Dates the grid has to contain: fixings, payments, exercise.
        virtual std::vector<Time> event_times() const = 0;
        /// Last date at which the trade can still pay.
        virtual Time maturity() const = 0;
        /// Value today, in domestic currency, in closed form.
        virtual Real value_today() const = 0;
        /// Called once, before any path, with the final grid.
        virtual void bind(const std::vector<Time> &grid) = 0;
        /**
         * @brief The values and cash flows of one path at every grid date.
         * @param values    value at each date of the flows strictly after it.
         * @param cashflows the flow of each date, > 0 received, or nullptr.
         */
        virtual void evaluate_path(const CrossCurrencyPath &path, std::size_t dates, Real *values,
                                   Real *cashflows) const = 0;

        /// Appends the trade to `terms`, for one unit of it.
        /// @return false, leaving `terms` untouched, when the trade has no
        ///         such description: its sensitivities are not computed.
        virtual bool describe(xva::TwoFactorTerms & /*terms*/) const { return false; }
    };

    /**
     * @brief A trade made of cash flows known today in each currency — an FX
     *        forward, a fixed-for-fixed cross-currency swap. At t,
     *
     *   V(t) = Σ_{T_j > t} c_d,j P_d(t, T_j) + S(t) Σ_{T_j > t} c_f,j P_f(t, T_j),
     *
     * exact given the state: every bond is a closed form of its rate.
     *
     * A European `VanillaOption` is an option on the exchange rate, settled
     * in cash in domestic currency on `notional` units, bought: Black's
     * formula on the forward of each path, with the variance that forward
     * still has to accumulate. With make_asset_model it is an option on an
     * asset under a stochastic rate (lot X14).
     *
     * @throws UnsupportedInstrument for anything else.
     */
    std::unique_ptr<CrossCurrencyFutureValue>
    make_cross_currency_future_value(const Instrument &instrument,
                                     const CrossCurrencyHullWhiteModel &model);

    /// A trade of one currency, valued on that currency's rate by the
    /// one-currency engine's own future value. A foreign one is converted at
    /// the spot of the path and of the date.
    /// @throws UnsupportedInstrument for a trade valued by regression.
    std::unique_ptr<CrossCurrencyFutureValue>
    in_domestic_currency(std::unique_ptr<FutureValue> trade);
    std::unique_ptr<CrossCurrencyFutureValue> in_foreign_currency(std::unique_ptr<FutureValue> trade,
                                                                  Real spot_today);

    /// The factors of every path, when the caller wants to look at them:
    /// element (path p, date i) is at `p * dates + i`.
    struct CrossCurrencyScenarios
    {
        std::vector<Real> domestic;
        std::vector<Real> foreign;
        std::vector<Real> spot;
    };

    /**
     * @brief Exposure engine with two currencies: one-factor Hull-White
     *        rates in each and a lognormal exchange rate
     *        (models/rates/cross_currency_hull_white.hpp).
     *
     * It fills the same cube as the one-currency engine, in domestic
     * currency and under the domestic T*-forward measure, so that netting,
     * collateral, the exposure metrics and the xVA report apply unchanged.
     * The three factors move by exact Gaussian transitions.
     *
     * **The domestic rate moves exactly as it does alone**: it takes the
     * draws the one-currency engine takes, so a netting set of domestic
     * trades has the same cube here, bit for bit.
     *
     * Closed forms only, on the CPU, under the pricing measure: no trade
     * valued by regression, no historical scenarios, no SIMM, no device.
     * The model must outlive the engine.
     */
    class CrossCurrencyExposureEngine
    {
      public:
        explicit CrossCurrencyExposureEngine(const CrossCurrencyHullWhiteModel &model);

        /// Adds an FX trade (forward, cross-currency swap); a negative
        /// quantity is the opposite position. Returns its index in the cube.
        std::size_t add(const Instrument &instrument, Real quantity = 1.0);
        /// Adds a rate trade denominated in the domestic or the foreign
        /// currency (a swap, a European swaption).
        std::size_t add_domestic(const Instrument &instrument, Real quantity = 1.0);
        std::size_t add_foreign(const Instrument &instrument, Real quantity = 1.0);
        std::size_t add(std::unique_ptr<CrossCurrencyFutureValue> trade, Real quantity = 1.0);

        std::size_t trades() const { return trades_.size(); }

        /// The grid simulate() will use.
        std::vector<Time> grid(const ExposureGridSettings &settings = {}) const;

        /**
         * @brief Runs the simulation.
         * @param scenarios when given, receives the factors of every path.
         * @throws InvalidInput without any trade, with zero paths, when the
         *         cube would exceed the memory limit, or when the settings
         *         ask for what this engine does not do (historical measure,
         *         SIMM, the GPU).
         */
        ExposurePaths simulate(const ExposureSimulationSettings &settings = {},
                               ThreadPool *pool = nullptr,
                               CrossCurrencyScenarios *scenarios = nullptr);

        /**
         * @brief CVA, DVA, FCA, FBA and the unilateral CVA of the netting set
         *        with their sensitivities to every input of the model, by
         *        adjoint differentiation: the two-factor counterpart of
         *        HullWhiteExposureEngine::xva_risks (§14.17), on the scenarios
         *        of simulate().
         *
         * The inputs: the zero rates of both curves, a and σ of both rates,
         * the spot, its volatility, the three correlations, the hazard rates,
         * the losses given default and the funding spreads. With
         * make_asset_model the foreign curve is the dividend yield, and the
         * foreign rate's own a, σ and correlations mean nothing.
         *
         * Covered so far: trades made of known cash flows (FX forwards,
         * fixed-for-fixed cross-currency swaps) and European options on the
         * exchange rate or the asset, uncollateralised.
         *
         * @throws InvalidInput for a trade of one currency (a swap, a
         *         swaption), under a CSA, or when simulate() would throw.
         */
        XvaRisks xva_risks(ExposureSimulationSettings settings, const XvaRiskInputs &inputs,
                           ThreadPool *pool = nullptr);

        /// The same adjustments on plain doubles, without sensitivities: what
        /// xva_risks() is checked against by finite differences.
        XvaValues xva_values(ExposureSimulationSettings settings, const XvaRiskInputs &inputs,
                             ThreadPool *pool = nullptr);

      private:
        struct Trade
        {
            std::unique_ptr<CrossCurrencyFutureValue> value;
            Real quantity;
        };

        /// What a run of xva_risks() or xva_values() is set up with
        /// (src/engines/xva/cross_currency_xva_risks.cpp).
        struct RiskRun;
        void prepare_risk_run(const ExposureSimulationSettings &settings, const XvaRiskInputs &inputs,
                              RiskRun &run) const;

        const CrossCurrencyHullWhiteModel &model_;
        std::vector<Trade> trades_;
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_CROSS_CURRENCY_EXPOSURE_ENGINE_HPP
