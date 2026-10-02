#include "quantModeling/risk/wrong_way_risk.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>

namespace quantModeling
{
    PathwiseSurvival wrong_way_survival(const ExposurePaths &paths,
                                        const std::vector<std::size_t> &trades,
                                        const CreditCurve &counterparty, Real b)
    {
        if (paths.measure != ExposureMeasure::RiskNeutral)
            throw InvalidInput("wrong-way risk: the hazard is calibrated to market prices and needs "
                               "a risk-neutral simulation");
        const std::size_t n = paths.dates(), N = paths.paths;
        if (n == 0 || N == 0 || paths.trades() == 0)
            throw InvalidInput("wrong-way risk: the simulation is empty");
        if (!std::isfinite(b))
            throw InvalidInput("wrong-way risk: b must be finite");
        std::vector<std::size_t> set = trades;
        if (set.empty())
        {
            set.resize(paths.trades());
            std::iota(set.begin(), set.end(), std::size_t{0});
        }
        for (const std::size_t k : set)
            if (k >= paths.trades())
                throw InvalidInput("wrong-way risk: trade index out of range");

        PathwiseSurvival out;
        out.times = paths.times;
        out.paths = N;
        out.b = b;
        out.survival.resize(N * n);
        if (b == 0.0)
        {
            // Independence: every path has the market's survival, exactly.
            for (std::size_t i = 0; i < n; ++i)
            {
                const Real s = counterparty.survival(paths.times[i]);
                for (std::size_t p = 0; p < N; ++p)
                    out.survival[p * n + i] = s;
            }
            return out;
        }

        out.a.resize(n);
        std::vector<Real> previous(N, 1.0), tilt(N);
        Time last = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Time dt = paths.times[i] - last;
            last = paths.times[i];
            // exp(b V), with the largest exponent taken out so that it cannot
            // overflow: a_i absorbs the shift.
            Real top = -std::numeric_limits<Real>::infinity();
            for (std::size_t p = 0; p < N; ++p)
            {
                Real v = 0.0;
                for (const std::size_t k : set)
                    v += paths.trade_values[k][p * n + i];
                tilt[p] = b * v;
                top = std::max(top, tilt[p]);
            }
            for (std::size_t p = 0; p < N; ++p)
                tilt[p] = std::exp(tilt[p] - top);

            // The market's survival, as the average over the paths.
            const Real target = counterparty.survival(paths.times[i]);
            // mean_p[q_{i-1} exp(-exp(c) tilt dt)] is decreasing in c:
            // bisection on c = a_i + top.
            const auto priced = [&](Real c)
            {
                const Real scale = std::exp(c) * dt;
                Real sum = 0.0;
                for (std::size_t p = 0; p < N; ++p)
                    sum += previous[p] * std::exp(-scale * tilt[p]);
                return sum / static_cast<Real>(N);
            };
            Real lo = -80.0, hi = 80.0;
            if (priced(lo) < target || priced(hi) > target)
                throw InvalidInput(
                    "wrong-way risk: no hazard level gives the market survival at t=" +
                    std::to_string(paths.times[i]) +
                    " (the survival curve rises there, or b is so large that the default "
                    "probability of the period would sit on a handful of paths)");
            for (int iteration = 0; iteration < 200 && hi - lo > 1e-13; ++iteration)
            {
                const Real mid = 0.5 * (lo + hi);
                (priced(mid) > target ? lo : hi) = mid;
            }
            const Real c = 0.5 * (lo + hi);
            out.a[i] = c - top;
            const Real scale = std::exp(c) * dt;
            for (std::size_t p = 0; p < N; ++p)
            {
                previous[p] *= std::exp(-scale * tilt[p]);
                out.survival[p * n + i] = previous[p];
            }
        }
        return out;
    }

} // namespace quantModeling
