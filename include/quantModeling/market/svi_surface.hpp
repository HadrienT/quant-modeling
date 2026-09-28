#ifndef MARKET_SVI_SURFACE_HPP
#define MARKET_SVI_SURFACE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi_calibration.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * A continuous total-variance surface built from independently calibrated
     * SVI slices, by linear interpolation in T at fixed log-moneyness k
     * between the two bracketing slices.
     *
     * Why linear in T: it is the standard, simplest choice that keeps the
     * surface calendar-consistent whenever the calibrated slices themselves
     * are (svi_slices_are_calendar_arbitrage_free on every adjacent pair) --
     * a linear interpolant between two curves that do not cross does not
     * cross either. It also makes every k-derivative the same linear
     * combination of the two slices' own analytic derivatives, since
     * differentiating in k and interpolating in T commute; no numerical
     * differentiation is needed anywhere in this class.
     *
     * Before the first slice, total variance grows linearly from zero:
     * w(k, T) = (T / T_1) w_1(k), the same linear interpolation with a
     * slice of zero variance at T = 0 -- implied vol held at the first
     * slice's, and a local vol whose total over [0, T_1] is exactly w_1.
     * Holding w_1 flat instead (as this class once did) left Dupire nothing
     * to read on [0, T_1], and the grid priced that interval with the next
     * segment's local vol: a one-year call then had a vega to the first two
     * slices, of opposite signs.
     *
     * Past the last slice, queries are clamped to it -- there is nothing to
     * extrapolate from. The grid this feeds to GridLocalVol
     * (market/dupire_from_svi.hpp) ends at ttm_max(), and GridLocalVol's
     * own flat extrapolation takes over beyond it -- one extrapolation
     * policy, not two.
     */
    class SVISurface
    {
      public:
        /// `slices` need at least 2 entries; sorted internally by ttm.
        explicit SVISurface(std::vector<SVISliceCalibration> slices);

        Real ttm_min() const noexcept { return slices_.front().ttm; }
        Real ttm_max() const noexcept { return slices_.back().ttm; }
        const std::vector<SVISliceCalibration> &slices() const noexcept { return slices_; }

        Real total_variance(Real k, Real T) const noexcept;     ///< w(k, T)
        Real total_variance_dk(Real k, Real T) const noexcept;  ///< dw/dk(k, T)
        Real total_variance_dk2(Real k, Real T) const noexcept; ///< d^2w/dk^2(k, T)

        /// Piecewise-constant in T: the slope of the segment T falls in,
        /// (w_{i+1}(k) - w_i(k)) / (T_{i+1} - T_i), and w_1(k) / T_1 before
        /// the first slice.
        Real total_variance_dT(Real k, Real T) const noexcept;

        Real implied_vol(Real k, Real T) const noexcept;

        /// The zero-variance slice at T = 0, as Bracket::lower.
        static constexpr std::size_t kZeroSlice = static_cast<std::size_t>(-1);

        /// The two slices T falls between and the weight of the upper one:
        /// w(k, T) = (1 - lambda) w_lower(k) + lambda w_upper(k), with
        /// w_kZeroSlice = 0, and dw/dT = (w_upper - w_lower) / width. T is
        /// clamped to [0, ttm_max()] first. Public so the superbucket
        /// (market/superbucket.hpp) replays the same decision on its tape.
        struct Bracket
        {
            std::size_t lower;
            std::size_t upper;
            Real lambda;
            Real width; ///< T_upper - T_lower
        };
        Bracket bracket(Real T) const noexcept;

      private:
        std::vector<SVISliceCalibration> slices_;

        /// f(k, slice) combined over the bracket of T: f is w, dw/dk or d2w/dk2.
        template <class F>
        Real combine(Real k, Real T, F f) const noexcept;
    };

} // namespace quantModeling

#endif
