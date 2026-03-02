#ifndef PRICERS_INPUTS_HPP
#define PRICERS_INPUTS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include <vector>

namespace quantModeling
{

    struct VanillaBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
        int tree_steps = 100;
        int pde_space_steps = 100;
        int pde_time_steps = 100;
    };

    struct AmericanVanillaBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;

        int tree_steps = 100;      // For binomial/trinomial trees
        int pde_space_steps = 100; // For PDE
        int pde_time_steps = 100;  // For PDE
    };

    struct AsianBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        AsianAverageType average_type = AsianAverageType::Arithmetic;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
    };

    struct EquityFutureInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real notional = 1.0;
    };

    struct BarrierBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        BarrierType barrier_type = BarrierType::DownAndOut;
        Real barrier_level = 0.0;
        Real rebate = 0.0;
        int n_steps = 0; ///< 0 = auto (52×T steps/yr, i.e. weekly monitoring)
        bool brownian_bridge = true;

        // Barrier uses 9 FD path variants per MC path (CRN Greeks),
        // so a lower default keeps latency manageable.
        int n_paths = 50000;
        int seed = 1;
        Real mc_epsilon = 0.0;
    };

    struct DigitalBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        DigitalPayoffType payoff_type = DigitalPayoffType::CashOrNothing;
        Real cash_amount = 1.0; ///< used only for CashOrNothing
    };

    struct LookbackBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        LookbackStyle style = LookbackStyle::FixedStrike;
        // extremum selects S_max or S_min for fixed-strike payoffs.
        // For floating-strike options the relevant extremum is derived from is_call
        // (Float Call -> S_min, Float Put -> S_max) and this field is ignored.
        LookbackExtremum extremum = LookbackExtremum::Maximum;
        int n_steps = 0; ///< monitoring steps per path; 0 = auto (252 × T)

        int n_paths = 200000;
        int seed = 1;
        bool mc_antithetic = true;
        Real mc_epsilon = 0.0; ///< reserved for future FD-Greeks use
    };

    struct BasketBSInput
    {
        /// Per-asset parameters (all vectors must have the same length n >= 2)
        std::vector<Real> spots;     ///< S0_i
        std::vector<Real> vols;      ///< sigma_i
        std::vector<Real> dividends; ///< q_i
        std::vector<Real> weights;   ///< w_i  (should sum to 1)
        /// Correlation matrix (n×n).  If empty, the identity matrix is used (zero pairwise correlation).
        std::vector<std::vector<Real>> correlations;

        Real strike = 100.0;
        Time maturity = 1.0;
        Real rate = 0.05;
        bool is_call = true;

        int n_paths = 200000;
        int seed = 1;
        bool mc_antithetic = true;
    };

    struct ZeroCouponBondInput
    {
        Time maturity;
        Real rate;
        Real notional = 1.0;
        std::vector<Time> discount_times;
        std::vector<Real> discount_factors;
    };

    struct FixedRateBondInput
    {
        Time maturity;
        Real rate;
        Real coupon_rate;
        int coupon_frequency = 1;
        Real notional = 1.0;
        std::vector<Time> discount_times;
        std::vector<Real> discount_factors;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Local-vol surface params — embedded inside every *LocalVol* input struct
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Reusable sub-struct carrying the Dupire σ_loc(K,T) grid.
     *
     * Embed this inside any instrument-specific LocalVol input struct.
     * Grid layout: sigma_loc_flat[i * T_grid.size() + j] = σ_loc(K_grid[i], T_grid[j]).
     */
    struct LocalVolSurface
    {
        std::vector<Real> K_grid;         ///< Strictly increasing strike axis
        std::vector<Real> T_grid;         ///< Strictly increasing time axis
        std::vector<Real> sigma_loc_flat; ///< K-major flattened σ_loc values
    };

    struct BarrierLocalVolInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        bool is_call;
        BarrierType barrier_type = BarrierType::DownAndOut;
        Real barrier_level = 0.0;
        Real rebate = 0.0;
        int n_steps = 0; ///< 0 = auto (52 × T steps/yr)
        bool brownian_bridge = true;

        LocalVolSurface surface;

        int n_paths = 50000;
        int seed = 1;
    };

    struct LookbackLocalVolInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        bool is_call;
        LookbackStyle style = LookbackStyle::FixedStrike;
        LookbackExtremum extremum = LookbackExtremum::Maximum;
        int n_steps = 0; ///< 0 = auto (252 × T steps/yr)

        LocalVolSurface surface;

        int n_paths = 200000;
        int seed = 1;
        bool mc_antithetic = true;
    };

    struct AsianLocalVolInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        bool is_call;
        AsianAverageType average_type = AsianAverageType::Arithmetic;

        LocalVolSurface surface;

        int n_paths = 200000;
        int seed = 1;
        bool mc_antithetic = true;
    };

    /**
     * @brief Input for Dupire local-vol MC pricing (European vanilla).
     *
     * The local-vol surface is passed as a pre-evaluated grid so that the
     * Python calibration pipeline (fetcher → cleaner → iv_surface → dupire)
     * can run in Python while the hot MC loop runs in C++.
     *
     * Grid layout: sigma_loc_flat[i * n_T + j] = σ_loc(K_grid[i], T_grid[j]).
     * The C++ engine bilinearly interpolates on this grid for every Euler step.
     */
    struct LocalVolInput
    {
        Real spot = 100.0;
        Real strike = 100.0;
        Real maturity = 1.0;
        Real rate = 0.05;
        Real dividend = 0.0;
        bool is_call = true;

        /// K axis of the local-vol grid (strictly increasing)
        std::vector<Real> K_grid;
        /// T axis of the local-vol grid (strictly increasing, ≥ 1 element)
        std::vector<Real> T_grid;
        /**
         * Flattened σ_loc values, row-major (K-major):
         *   sigma_loc_flat[i * T_grid.size() + j] = σ_loc(K_grid[i], T_grid[j])
         */
        std::vector<Real> sigma_loc_flat;

        int n_paths = 50000;
        int n_steps_per_year = 252;
        int seed = 1;
        bool mc_antithetic = true;
        bool compute_greeks = true;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Short-rate model inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing a ZCB under a short-rate model (Vasicek/CIR/HW).
     */
    struct ShortRateZCBInput
    {
        std::string model_type; ///< "vasicek", "cir", or "hull_white"
        Real a;                 ///< mean reversion speed
        Real b;                 ///< long-term mean rate
        Real sigma;             ///< volatility
        Real r0;                ///< initial short rate
        Time maturity;          ///< bond maturity
        Real notional = 1.0;
    };

    /**
     * @brief Input for pricing a fixed-rate bond under a short-rate model.
     */
    struct ShortRateBondInput
    {
        std::string model_type;
        Real a;
        Real b;
        Real sigma;
        Real r0;
        Time maturity;
        Real coupon_rate;
        int coupon_frequency = 1;
        Real notional = 1.0;
    };

    /**
     * @brief Input for pricing a European bond option under a short-rate model.
     */
    struct ShortRateBondOptionInput
    {
        std::string model_type;
        Real a;
        Real b;
        Real sigma;
        Real r0;
        Time option_maturity; ///< option expiry T
        Time bond_maturity;   ///< underlying ZCB maturity S  (S > T)
        Real strike;          ///< option strike K
        bool is_call = true;
        Real notional = 1.0;

        int n_paths = 200000; ///< MC paths (used only by MC engine)
        int seed = 1;
    };

    /**
     * @brief Input for pricing a cap or floor under a short-rate model.
     */
    struct ShortRateCapFloorInput
    {
        std::string model_type;
        Real a;
        Real b;
        Real sigma;
        Real r0;
        std::vector<Time> schedule; ///< tenor schedule [T_0, ..., T_n]
        Real strike;                ///< cap/floor rate K
        bool is_cap = true;
        Real notional = 1.0;

        int n_paths = 200000;
        int seed = 1;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Structured products inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing an Autocallable note under Black-Scholes.
     */
    struct AutocallBSInput
    {
        Real spot;
        Real rate;
        Real dividend = 0.0;
        Real vol;
        std::vector<Time> observation_dates; ///< T_1, ..., T_n  (sorted, > 0)
        Real autocall_barrier;               ///< fraction of S0 (e.g. 1.0 = ATM)
        Real coupon_barrier;                 ///< fraction of S0 for coupon trigger
        Real put_barrier;                    ///< fraction of S0 for knock-in put
        Real coupon_rate;                    ///< per-period coupon as fraction of notional
        Real notional = 1000.0;
        bool memory_coupon = true;
        bool ki_continuous = false; ///< continuous vs. terminal KI monitoring

        int n_paths = 200000;
        int seed = 1;
    };

    /**
     * @brief Input for pricing a Himalaya (Mountain) option under multi-asset BS.
     */
    struct MountainBSInput
    {
        std::vector<Real> spots;     ///< S0_i
        std::vector<Real> vols;      ///< σ_i
        std::vector<Real> dividends; ///< q_i
        /// Correlation matrix (n×n).  Empty → identity.
        std::vector<std::vector<Real>> correlations;

        std::vector<Time> observation_dates; ///< one per asset
        Real strike = 0.0;                   ///< strike on average return
        bool is_call = true;
        Real rate = 0.05;
        Real notional = 100.0;

        int n_paths = 200000;
        int seed = 1;
    };

    /**
     * @brief Input for pricing a standalone Caplet/Floorlet under a short-rate model.
     */
    struct ShortRateCapletInput
    {
        std::string model_type; ///< "vasicek", "cir", or "hull_white"
        Real a;
        Real b;
        Real sigma;
        Real r0;
        Time start;  ///< caplet fixing date
        Time end;    ///< caplet payment date
        Real strike; ///< cap/floor rate K
        bool is_cap = true;
        Real notional = 1.0;

        int n_paths = 200000; ///< MC paths (used only by MC engine)
        int seed = 1;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Volatility products inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing a variance swap under Black-Scholes.
     */
    struct VarianceSwapBSInput
    {
        Real spot;
        Real rate;
        Real dividend = 0.0;
        Real vol;
        Time maturity;
        Real strike_var; ///< K_var (annualised variance strike)
        Real notional = 100.0;
        std::vector<Time> observation_dates; ///< optional discrete schedule

        int n_paths = 200000;
        int seed = 1;
    };

    /**
     * @brief Input for pricing a volatility swap under Black-Scholes.
     */
    struct VolatilitySwapBSInput
    {
        Real spot;
        Real rate;
        Real dividend = 0.0;
        Real vol;
        Time maturity;
        Real strike_vol; ///< K_vol (annualised vol strike)
        Real notional = 100.0;
        std::vector<Time> observation_dates; ///< optional discrete schedule

        int n_paths = 200000;
        int seed = 1;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Dispersion products inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing a dispersion swap under multi-asset BS.
     */
    struct DispersionBSInput
    {
        std::vector<Real> spots;                     ///< S0_i
        std::vector<Real> vols;                      ///< σ_i
        std::vector<Real> dividends;                 ///< q_i
        std::vector<Real> weights;                   ///< index constituent weights (sum to 1)
        std::vector<std::vector<Real>> correlations; ///< n×n correlation matrix

        Time maturity;
        Real strike_spread = 0.0; ///< K_spread (annualised variance spread)
        Real rate = 0.05;
        Real notional = 100.0;
        std::vector<Time> observation_dates; ///< optional

        int n_paths = 200000;
        int seed = 1;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  FX products inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing an FX forward.
     */
    struct FXForwardInput
    {
        Real spot;          ///< spot FX rate (domestic per foreign)
        Real rate_domestic; ///< domestic risk-free rate
        Real rate_foreign;  ///< foreign risk-free rate
        Real vol;           ///< FX volatility (not used for forward, included for consistency)
        Real strike;        ///< delivery rate K
        Time maturity;
        Real notional = 1.0;
    };

    /**
     * @brief Input for pricing a European FX option (Garman-Kohlhagen).
     */
    struct FXOptionInput
    {
        Real spot;
        Real rate_domestic;
        Real rate_foreign;
        Real vol;
        Real strike;
        Time maturity;
        bool is_call = true;
        Real notional = 1.0;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Commodity products inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing a commodity forward.
     */
    struct CommodityForwardInput
    {
        Real spot;
        Real rate;
        Real storage_cost = 0.0;      ///< annualised storage cost u
        Real convenience_yield = 0.0; ///< annualised convenience yield y
        Real vol;                     ///< forward vol (not used for forward)
        Real strike;
        Time maturity;
        Real notional = 1.0;
    };

    /**
     * @brief Input for pricing a European commodity option (Black '76).
     */
    struct CommodityOptionInput
    {
        Real spot;
        Real rate;
        Real storage_cost = 0.0;
        Real convenience_yield = 0.0;
        Real vol;
        Real strike;
        Time maturity;
        bool is_call = true;
        Real notional = 1.0;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Rainbow (worst-of / best-of) options inputs
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Input for pricing worst-of or best-of options under multi-asset BS.
     */
    struct RainbowBSInput
    {
        std::vector<Real> spots;                     ///< S0_i
        std::vector<Real> vols;                      ///< σ_i
        std::vector<Real> dividends;                 ///< q_i
        std::vector<std::vector<Real>> correlations; ///< n×n (empty → identity)

        Time maturity;
        Real strike = 1.0; ///< performance strike (1.0 = ATM)
        bool is_call = true;
        Real rate = 0.05;
        Real notional = 100.0;

        int n_paths = 200000;
        int seed = 1;
    };

} // namespace quantModeling

#endif
