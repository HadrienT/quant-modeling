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

        for (std::size_t i = 1; i < slices_.size(); ++i)
            if (!(slices_[i].ttm > slices_[i - 1].ttm))
                throw InvalidInput("SVISurface: slice maturities must be strictly increasing and distinct");
    }

    SVISurface::Bracket SVISurface::bracket(Real T) const noexcept
    {
        const Real clamped = std::clamp(T, ttm_min(), ttm_max());

        std::size_t i = 0;
        while (i + 2 < slices_.size() && slices_[i + 1].ttm < clamped)
            ++i;

        const Real T0 = slices_[i].ttm, T1 = slices_[i + 1].ttm;
        const Real lambda = (clamped - T0) / (T1 - T0);
        return {i, lambda};
    }

    Real SVISurface::total_variance(Real k, Real T) const noexcept
    {
        const Bracket br = bracket(T);
        const Real w0 = svi_total_variance(k, slices_[br.i].params);
        const Real w1 = svi_total_variance(k, slices_[br.i + 1].params);
        return (1.0 - br.lambda) * w0 + br.lambda * w1;
    }

    Real SVISurface::total_variance_dk(Real k, Real T) const noexcept
    {
        const Bracket br = bracket(T);
        const Real d0 = svi_total_variance_dk(k, slices_[br.i].params);
        const Real d1 = svi_total_variance_dk(k, slices_[br.i + 1].params);
        return (1.0 - br.lambda) * d0 + br.lambda * d1;
    }

    Real SVISurface::total_variance_dk2(Real k, Real T) const noexcept
    {
        const Bracket br = bracket(T);
        const Real d0 = svi_total_variance_dk2(k, slices_[br.i].params);
        const Real d1 = svi_total_variance_dk2(k, slices_[br.i + 1].params);
        return (1.0 - br.lambda) * d0 + br.lambda * d1;
    }

    Real SVISurface::total_variance_dT(Real k, Real T) const noexcept
    {
        const Bracket br = bracket(T);
        const Real w0 = svi_total_variance(k, slices_[br.i].params);
        const Real w1 = svi_total_variance(k, slices_[br.i + 1].params);
        return (w1 - w0) / (slices_[br.i + 1].ttm - slices_[br.i].ttm);
    }

    Real SVISurface::implied_vol(Real k, Real T) const noexcept
    {
        const Real w = total_variance(k, T);
        return std::sqrt(std::max(w, Real(0.0)) / T);
    }

} // namespace quantModeling
