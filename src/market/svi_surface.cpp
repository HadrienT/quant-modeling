#include "quantModeling/market/svi_surface.hpp"

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    SVISurface::SVISurface(std::vector<SVISliceCalibration> slices)
        : slices_(std::move(slices))
    {
        if (slices_.size() < 2)
            throw InvalidInput("SVISurface: at least 2 calibrated slices are required");

        std::sort(slices_.begin(), slices_.end(),
                  [](const SVISliceCalibration &a, const SVISliceCalibration &b)
                  { return a.ttm < b.ttm; });

        if (!(slices_.front().ttm > 0.0))
            throw InvalidInput("SVISurface: slice maturities must be positive");
        for (std::size_t i = 1; i < slices_.size(); ++i)
            if (!(slices_[i].ttm > slices_[i - 1].ttm))
                throw InvalidInput("SVISurface: slice maturities must be strictly increasing and distinct");
    }

    SVISurface::Bracket SVISurface::bracket(Real T) const noexcept
    {
        const Real clamped = std::clamp(T, Real(0.0), ttm_max());
        if (clamped <= ttm_min()) // T_1 itself closes [0, T_1], as every T_i closes its segment
            return {kZeroSlice, 0, clamped / ttm_min(), ttm_min()};

        std::size_t i = 0;
        while (i + 2 < slices_.size() && slices_[i + 1].ttm < clamped)
            ++i;

        const Real T0 = slices_[i].ttm, T1 = slices_[i + 1].ttm;
        return {i, i + 1, (clamped - T0) / (T1 - T0), T1 - T0};
    }

    template <class F>
    Real SVISurface::combine(Real k, Real T, F f) const noexcept
    {
        const Bracket br = bracket(T);
        const Real upper = br.lambda * f(k, slices_[br.upper].params);
        if (br.lower == kZeroSlice)
            return upper;
        return (1.0 - br.lambda) * f(k, slices_[br.lower].params) + upper;
    }

    Real SVISurface::total_variance(Real k, Real T) const noexcept
    {
        return combine(k, T, [](Real x, const SVIParams &p)
                       { return svi_total_variance(x, p); });
    }

    Real SVISurface::total_variance_dk(Real k, Real T) const noexcept
    {
        return combine(k, T, [](Real x, const SVIParams &p)
                       { return svi_total_variance_dk(x, p); });
    }

    Real SVISurface::total_variance_dk2(Real k, Real T) const noexcept
    {
        return combine(k, T, [](Real x, const SVIParams &p)
                       { return svi_total_variance_dk2(x, p); });
    }

    Real SVISurface::total_variance_dT(Real k, Real T) const noexcept
    {
        const Bracket br = bracket(T);
        const Real w1 = svi_total_variance(k, slices_[br.upper].params);
        const Real w0 = br.lower == kZeroSlice ? 0.0 : svi_total_variance(k, slices_[br.lower].params);
        return (w1 - w0) / br.width;
    }

    Real SVISurface::implied_vol(Real k, Real T) const noexcept
    {
        const Real w = total_variance(k, T);
        return std::sqrt(std::max(w, Real(0.0)) / T);
    }

} // namespace quantModeling
