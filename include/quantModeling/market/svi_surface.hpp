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
     * Only defined on [ttm_min(), ttm_max()] -- there is nothing to
     * extrapolate from beyond the calibrated maturities. The grid this feeds
     * to GridLocalVol (market/dupire_from_svi.hpp) is built on exactly that
     * range, and GridLocalVol's own flat extrapolation at the boundaries
     * takes over for any query outside it -- one extrapolation policy, not
     * two.
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
        /// (w_{i+1}(k) - w_i(k)) / (T_{i+1} - T_i).
        Real total_variance_dT(Real k, Real T) const noexcept;

        Real implied_vol(Real k, Real T) const noexcept;

      private:
        std::vector<SVISliceCalibration> slices_;

        /// Index i and interpolation weight lambda such that T falls in
        /// [slices_[i].ttm, slices_[i+1].ttm] (T clamped to the surface's
        /// range first).
        struct Bracket
        {
            std::size_t i;
            Real lambda;
        };
        Bracket bracket(Real T) const noexcept;
    };

} // namespace quantModeling

#endif
