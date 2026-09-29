#ifndef ENGINE_MC_ROUGH_BERGOMI_SURFACE_HPP
#define ENGINE_MC_ROUGH_BERGOMI_SURFACE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/rough_bergomi.hpp"

#include <cstdint>
#include <vector>

namespace quantModeling
{

    /**
     * @brief The forward variance curve ξ0(t) = E[V_t] of rough Bergomi,
     *        piecewise constant: ξ0 = xi[i] on (times[i-1], times[i]], the
     *        last value beyond (blueprint: etc/roadmap.md §1c).
     */
    struct ForwardVarianceCurve
    {
        std::vector<Time> times; ///< right ends, strictly increasing
        std::vector<Real> xi;    ///< > 0

        Real operator()(Time t) const;

        /// ξ0 from total variances w(T) at a few maturities -- the variance
        /// swap's, E[∫V] = −2 E[ln S_T/F] (market/rough_bergomi_calibration.hpp):
        /// ξ0 = Δw / ΔT between them, w(0) = 0. A calendar-arbitraged pair
        /// (w falling) is floored at a tenth of the average variance rather
        /// than made negative, and counted in `floored`.
        static ForwardVarianceCurve from_total_variance(const std::vector<Time> &maturities,
                                                        const std::vector<Real> &total_variance,
                                                        int *floored = nullptr);
    };

    struct RoughBergomiSurfaceSettings
    {
        int steps_per_year = 252;
        int n_paths = 20000; ///< antithetic pairs count as two paths
        std::uint64_t seed = 1;
        int max_threads = 0; ///< 0: every available CPU
    };

    /// One option of the surface: maturity and log-moneyness k = ln(K / F_T).
    struct RoughBergomiQuote
    {
        Time ttm;
        Real k;
    };

    struct RoughBergomiSurface
    {
        /// Forward-normalised price of the out-of-the-money option of each
        /// quote (call for k ≥ 0, put below): E[(X_T − e^k)^+] or
        /// E[(e^k − X_T)^+], X = S / F, with X_T as a control variate.
        std::vector<Real> otm_prices;
        std::vector<Real> std_errors;
        /// Black implied vol of each quote (NaN when the price is outside
        /// the no-arbitrage bounds or has no time value).
        std::vector<Real> implied_vols;
    };

    /**
     * @brief Price a whole surface of vanillas under rough Bergomi from one
     *        set of paths: the hybrid scheme of engines/mc/rough_bergomi_hybrid.hpp
     *        (Bennedsen-Lunde-Pakkanen, κ = 1) on a uniform grid of
     *        1/steps_per_year up to the longest maturity, each maturity read
     *        at its nearest grid date (≤ half a step away), with a forward
     *        variance curve instead of a flat ξ0.
     *
     * Path p draws from Philox(seed, p) and is followed by its antithetic
     * mirror; paths are split into fixed blocks summed in block order, so the
     * result is the same bits for any thread count — what makes a
     * calibration objective deterministic (common random numbers across
     * parameter values).
     */
    RoughBergomiSurface rough_bergomi_surface(const RoughBergomiParams &params, const ForwardVarianceCurve &xi,
                                              const std::vector<RoughBergomiQuote> &quotes,
                                              const RoughBergomiSurfaceSettings &settings = {});

} // namespace quantModeling

#endif
