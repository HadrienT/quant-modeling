#include "quantModeling/market/multi_curve_bootstrap.hpp"

#include "quantModeling/engines/analytic/swap.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace quantModeling
{
    namespace
    {
        /// One quote to fit: its maturity (the new pillar) and its residual on
        /// a candidate projection curve (zero when the curve reprices it).
        struct PillarQuote
        {
            Time maturity;
            std::function<Real(const DiscountCurve &)> residual;
        };

        Real solve_pillar(const std::vector<Time> &times, const std::vector<Real> &dfs,
                          const PillarQuote &q)
        {
            const auto at = [&](Real z)
            {
                std::vector<Time> t = times;
                std::vector<Real> d = dfs;
                t.push_back(q.maturity);
                d.push_back(std::exp(-z * q.maturity));
                return q.residual(DiscountCurve(std::move(t), std::move(d), CurveExtrapolation::FlatForward));
            };
            Real lo = -1.0, hi = 3.0;
            const Real f_lo = at(lo), f_hi = at(hi);
            if (!(f_lo * f_hi < 0.0))
                throw InvalidInput("bootstrap_projection_curve: no zero rate in [-100%, 300%] "
                                   "reprices the quote at t=" +
                                   std::to_string(q.maturity));
            const bool increasing = f_hi > 0.0;
            for (int iter = 0; iter < 200 && hi - lo > 1e-15; ++iter)
            {
                const Real mid = 0.5 * (lo + hi);
                if ((at(mid) > 0.0) == increasing)
                    hi = mid;
                else
                    lo = mid;
            }
            return 0.5 * (lo + hi);
        }
    } // namespace

    ParRateQuote make_ois_quote(Time maturity, Real rate)
    {
        if (!(maturity > 0.0))
            throw InvalidInput("make_ois_quote: maturity must be > 0");
        ParRateQuote q;
        q.rate = rate;
        // Annual coupons, a short first period when the tenor is not whole
        // years (the stub sits at the front, as schedules generated backward).
        const long whole = static_cast<long>(std::floor(maturity + 1e-9));
        const Time stub = maturity - static_cast<Real>(whole);
        Time prev = 0.0;
        if (stub > 1e-9)
        {
            q.payment_times.push_back(stub);
            q.accruals.push_back(stub);
            prev = stub;
        }
        for (long i = 1; i <= whole; ++i)
        {
            const Time t = stub + static_cast<Real>(i);
            q.payment_times.push_back(t);
            q.accruals.push_back(t - prev);
            prev = t;
        }
        return q;
    }

    ProjectionSwapQuote make_projection_swap_quote(Time tenor, Real rate, int fixed_frequency,
                                                   int float_frequency)
    {
        return {make_swap(0.0, tenor, rate, fixed_frequency, float_frequency)};
    }

    DiscountCurve bootstrap_projection_curve(const DiscountCurve &discount, std::vector<FraQuote> fras,
                                             std::vector<ProjectionSwapQuote> swaps)
    {
        std::vector<PillarQuote> quotes;
        for (const FraQuote &f : fras)
        {
            if (!(f.end > f.start) || f.start < 0.0 || !(f.accrual > 0.0))
                throw InvalidInput("bootstrap_projection_curve: a FRA needs 0 <= start < end and accrual > 0");
            quotes.push_back({f.end, [f](const DiscountCurve &proj)
                              { return forward_rate(proj, f.start, f.end, f.accrual) - f.rate; }});
        }
        for (const ProjectionSwapQuote &s : swaps)
        {
            quotes.push_back({s.maturity(), [&discount, s](const DiscountCurve &proj)
                              {
                                  const SwapValuation v = value_swap(s.swap, MultiCurve{discount, proj});
                                  return v.par_rate - s.swap.fixed_rate;
                              }});
        }
        if (quotes.empty())
            throw InvalidInput("bootstrap_projection_curve: need at least one quote");
        std::sort(quotes.begin(), quotes.end(),
                  [](const PillarQuote &a, const PillarQuote &b) { return a.maturity < b.maturity; });

        std::vector<Time> times;
        std::vector<Real> dfs;
        for (const PillarQuote &q : quotes)
        {
            if (!(q.maturity > 0.0))
                throw InvalidInput("bootstrap_projection_curve: maturities must be > 0");
            if (!times.empty() && !(q.maturity > times.back() + 1e-12))
                throw InvalidInput("bootstrap_projection_curve: duplicate maturity t=" +
                                   std::to_string(q.maturity));
            const Real z = solve_pillar(times, dfs, q);
            times.push_back(q.maturity);
            dfs.push_back(std::exp(-z * q.maturity));
        }
        return DiscountCurve(std::move(times), std::move(dfs), CurveExtrapolation::FlatForward);
    }

} // namespace quantModeling
