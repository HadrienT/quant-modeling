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

} // namespace quantModeling

#endif
