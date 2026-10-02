#include "quantModeling/engines/analytic/hull_white_swaption.hpp"

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;

        /// Exercise value at `t` of the bond portfolio, given x(t).
        Real bonds_value(const std::vector<BondCashflow> &cfs, Time t, Real x,
                         const HullWhiteCurveModel &model)
        {
            Real v = 0.0;
            for (const BondCashflow &c : cfs)
                v += c.amount * model.zcb(t, c.time, x);
            return v;
        }

        /// E[L(X)], X ~ N(m, s²), L the piecewise-linear interpolant of
        /// (xs, us) extrapolated linearly past both ends — exact.
        Real expect_piecewise_linear(const std::vector<Real> &xs, const std::vector<Real> &us, Real m, Real s)
        {
            const std::size_t n = xs.size();
            // ∫_l^u (α + β x) dN(m, s²) = (α + β m)[Φ(zu) − Φ(zl)] + β s [φ(zl) − φ(zu)].
            const auto piece = [m, s](Real alpha, Real beta, Real Pl, Real Pu, Real pl, Real pu)
            { return (alpha + beta * m) * (Pu - Pl) + beta * s * (pl - pu); };
            const auto z = [m, s](Real x)
            { return (x - m) / s; };

            // Only cells within ±10 sd of the mean carry weight; the tails beyond
            // them go to the extrapolated end segments.
            const Real lo_x = m - 10.0 * s, hi_x = m + 10.0 * s;
            std::size_t first = static_cast<std::size_t>(
                std::lower_bound(xs.begin(), xs.end(), lo_x) - xs.begin());
            std::size_t last = static_cast<std::size_t>(
                std::upper_bound(xs.begin(), xs.end(), hi_x) - xs.begin());
            first = first > 0 ? first - 1 : 0;
            last = std::min(last, n - 1);
            first = std::min(first, last);

            Real total = 0.0;
            // Left tail (−∞, x_first] on the first cell's line.
            {
                const std::size_t j = std::min(first, n - 2);
                const Real beta = (us[j + 1] - us[j]) / (xs[j + 1] - xs[j]);
                const Real alpha = us[j] - beta * xs[j];
                const Real zu = z(xs[first]);
                total += piece(alpha, beta, 0.0, norm_cdf(zu), 0.0, norm_pdf(zu));
            }
            const Real z0 = z(xs[first]);
            Real Pl = norm_cdf(z0), pl = norm_pdf(z0);
            for (std::size_t j = first; j < last; ++j)
            {
                const Real beta = (us[j + 1] - us[j]) / (xs[j + 1] - xs[j]);
                const Real alpha = us[j] - beta * xs[j];
                const Real zu = z(xs[j + 1]);
                const Real Pu = norm_cdf(zu), pu = norm_pdf(zu);
                total += piece(alpha, beta, Pl, Pu, pl, pu);
                Pl = Pu;
                pl = pu;
            }
            // Right tail [x_last, ∞) on the last cell's line.
            {
                const std::size_t j = std::max<std::size_t>(last, 1) - 1;
                const Real beta = (us[j + 1] - us[j]) / (xs[j + 1] - xs[j]);
                const Real alpha = us[j] - beta * xs[j];
                total += piece(alpha, beta, Pl, 1.0, pl, 0.0);
            }
            return total;
        }
    } // namespace

    std::vector<BondCashflow> swap_as_bonds(const InterestRateSwap &swap, Time exercise,
                                            const HullWhiteCurveModel &model)
    {
        const InterestRateSwap tail = swap.tail_from(exercise);
        const Real side = swap.payer ? 1.0 : -1.0;
        const Real N = swap.notional;
        const DiscountCurve &d = model.discount();
        const DiscountCurve &p = model.projection();
        std::vector<BondCashflow> cfs;
        for (const CouponPeriod &c : tail.fixed_leg)
            cfs.push_back({c.payment, -side * N * swap.fixed_rate * c.accrual});
        for (const CouponPeriod &c : tail.floating_leg)
        {
            if (std::abs(c.payment - c.end) > kTimeEps)
                throw InvalidInput("Hull-White swaption: floating coupons must pay on their period end");
            const Real beta = (p.discount(c.start) / p.discount(c.end)) / (d.discount(c.start) / d.discount(c.end));
            cfs.push_back({c.start, side * N * beta});
            cfs.push_back({c.end, -side * N * (1.0 - swap.spread * c.accrual)});
        }
        return cfs;
    }

    HullWhiteExerciseRegion hull_white_exercise_region(const Swaption &swaption,
                                                       const HullWhiteCurveModel &model)
    {
        const Time T = swaption.expiry;
        if (swaption.swap.start() < T - kTimeEps)
            throw InvalidInput("Hull-White swaption: the swap must start on or after the expiry");
        HullWhiteExerciseRegion region;
        region.expiry = T;
        region.bonds = swap_as_bonds(swaption.swap, T, model);
        const std::vector<BondCashflow> &cfs = region.bonds;
        if (cfs.empty())
            return region;

        const Real P0T = model.discount().discount(T);
        for (const BondCashflow &c : cfs)
        {
            region.forward_bond.push_back(model.discount().discount(c.time) / P0T);
            region.G.push_back(model.G(T, c.time));
        }
        if (!(T > kTimeEps))
        {
            // Expiring today: exercised or not, whatever x.
            if (bonds_value(cfs, 0.0, 0.0, model) > 0.0)
                region.intervals.push_back({-INFINITY, INFINITY});
            return region;
        }

        const Real sd = std::sqrt(model.y(T));
        const auto g = [&](Real x)
        { return bonds_value(cfs, T, x, model); };

        constexpr int n = 480;
        const Real lo = -12.0 * sd, hi = 12.0 * sd, h = (hi - lo) / n;
        Real x_prev = lo, g_prev = g(lo);
        Real region_start = g_prev > 0.0 ? -INFINITY : NAN;
        for (int i = 1; i <= n; ++i)
        {
            const Real x = lo + i * h, gx = g(x);
            if ((g_prev > 0.0) != (gx > 0.0))
            {
                Real a = x_prev, b = x;
                const bool rising = gx > 0.0;
                for (int it = 0; it < 200 && b - a > 1e-15 * (1.0 + std::abs(a)); ++it)
                {
                    const Real mid = 0.5 * (a + b);
                    if ((g(mid) > 0.0) == rising)
                        b = mid;
                    else
                        a = mid;
                }
                const Real root = 0.5 * (a + b);
                if (rising)
                    region_start = root;
                else
                    region.intervals.push_back({region_start, root});
            }
            x_prev = x;
            g_prev = gx;
        }
        if (g_prev > 0.0)
            region.intervals.push_back({region_start, INFINITY});
        return region;
    }

    Real hull_white_european_swaption(const HullWhiteExerciseRegion &region,
                                      const HullWhiteCurveModel &model, Time t, Real x)
    {
        const std::vector<BondCashflow> &cfs = region.bonds;
        if (cfs.empty())
            return 0.0;
        const Time T = region.expiry;
        if (t > T + kTimeEps)
            throw InvalidInput("Hull-White swaption: valuation date past the expiry");
        if (!(T - t > kTimeEps))
            return std::max(bonds_value(cfs, T, x, model), 0.0);

        // Under the T-forward measure x(T) | x(t) ~ N(m, v) -- N(0, y(T)) seen
        // from today -- and for each bond
        //   E[P(T, t_k) 1{x(T) in (l, u)}]
        //     = A_k e^{-G_k m + G_k² (v - y(T)) / 2}
        //       [Φ((u - m + G_k v)/sd) - Φ((l - m + G_k v)/sd)].
        const auto tr = model.transition(t, T, T);
        const Real m = tr.decay * x + tr.drift, v = tr.variance, sd = std::sqrt(v);
        const Real convexity = 0.5 * (v - model.y(T));
        Real value = 0.0;
        for (const auto &[l, u] : region.intervals)
        {
            for (std::size_t k = 0; k < cfs.size(); ++k)
            {
                const Real G = region.G[k];
                const Real Fu = std::isinf(u) ? 1.0 : norm_cdf((u - m + G * v) / sd);
                const Real Fl = std::isinf(l) ? 0.0 : norm_cdf((l - m + G * v) / sd);
                value += cfs[k].amount * region.forward_bond[k] *
                         std::exp(-G * m + G * G * convexity) * (Fu - Fl);
            }
        }
        // Deep out of the money the terms cancel down to rounding noise,
        // which may come out as -1e-18: an option is not worth less than zero.
        return std::max(model.zcb(t, T, x) * value, 0.0);
    }

    void hull_white_european_swaption_terms(const HullWhiteExerciseRegion &region,
                                            const HullWhiteCurveModel &model, Time t, Real x,
                                            std::vector<Real> &terms)
    {
        const std::vector<BondCashflow> &cfs = region.bonds;
        terms.assign(cfs.size(), 0.0);
        if (cfs.empty())
            return;
        const Time T = region.expiry;
        if (t > T + kTimeEps)
            throw InvalidInput("Hull-White swaption: valuation date past the expiry");
        if (!(T - t > kTimeEps))
        {
            // At the expiry: the swap's own bonds, when it is exercised.
            if (bonds_value(cfs, T, x, model) > 0.0)
                for (std::size_t k = 0; k < cfs.size(); ++k)
                    terms[k] = cfs[k].amount * model.zcb(T, cfs[k].time, x);
            return;
        }
        // The sum of hull_white_european_swaption, kept apart per bond.
        const auto tr = model.transition(t, T, T);
        const Real m = tr.decay * x + tr.drift, v = tr.variance, sd = std::sqrt(v);
        const Real convexity = 0.5 * (v - model.y(T));
        const Real discount = model.zcb(t, T, x);
        for (const auto &[l, u] : region.intervals)
            for (std::size_t k = 0; k < cfs.size(); ++k)
            {
                const Real G = region.G[k];
                const Real Fu = std::isinf(u) ? 1.0 : norm_cdf((u - m + G * v) / sd);
                const Real Fl = std::isinf(l) ? 0.0 : norm_cdf((l - m + G * v) / sd);
                terms[k] += discount * cfs[k].amount * region.forward_bond[k] *
                            std::exp(-G * m + G * G * convexity) * (Fu - Fl);
            }
    }

    Real hull_white_european_swaption(const Swaption &swaption, const HullWhiteCurveModel &model)
    {
        return hull_white_european_swaption(hull_white_exercise_region(swaption, model), model,
                                            0.0, 0.0);
    }

    Real hull_white_bermudan_swaption(const BermudanSwaption &bermudan, const HullWhiteCurveModel &model,
                                      const HullWhiteLatticeSettings &settings)
    {
        const InterestRateSwap &swap = bermudan.swap;
        const Time TN = swap.maturity();
        std::vector<Time> ex;
        for (const Time t : bermudan.exercise_times)
        {
            if (!(t > 0.0))
                throw InvalidInput("Bermudan swaption: exercise times must be > 0");
            if (!ex.empty() && !(t > ex.back()))
                throw InvalidInput("Bermudan swaption: exercise times must be strictly increasing");
            if (!swap.tail_from(t).fixed_leg.empty())
                ex.push_back(t);
        }
        if (ex.empty())
            return 0.0;
        if (settings.grid_points < 3 || settings.grid_points % 2 == 0)
            throw InvalidInput("Bermudan swaption: grid_points must be odd and >= 3");

        std::vector<std::vector<BondCashflow>> exercise_bonds;
        for (const Time t : ex)
            exercise_bonds.push_back(swap_as_bonds(swap, t, model));

        // Grid around the terminal-measure mean of x at the last exercise.
        const auto last = model.transition(0.0, ex.back(), TN);
        const Real centre = last.drift, half = settings.grid_std_devs * std::sqrt(last.variance);
        const auto n = static_cast<std::size_t>(settings.grid_points);
        std::vector<Real> xs(n);
        for (std::size_t j = 0; j < n; ++j)
            xs[j] = centre - half + 2.0 * half * static_cast<Real>(j) / static_cast<Real>(n - 1);

        // Deflated exercise value max(swap, 0) / P(t, T_N) at date i on the grid.
        const auto deflated_exercise = [&](std::size_t i, Real x)
        {
            return bonds_value(exercise_bonds[i], ex[i], x, model) / model.zcb(ex[i], TN, x);
        };

        std::vector<Real> U(n);
        for (std::size_t j = 0; j < n; ++j)
            U[j] = std::max(deflated_exercise(ex.size() - 1, xs[j]), 0.0);

        std::vector<Real> next(n);
        for (std::size_t i = ex.size() - 1; i-- > 0;)
        {
            const auto tr = model.transition(ex[i], ex[i + 1], TN);
            const Real s = std::sqrt(tr.variance);
            for (std::size_t j = 0; j < n; ++j)
            {
                const Real cont = expect_piecewise_linear(xs, U, tr.decay * xs[j] + tr.drift, s);
                next[j] = std::max(cont, deflated_exercise(i, xs[j]));
            }
            U.swap(next);
        }
        const auto first = model.transition(0.0, ex.front(), TN);
        return model.discount().discount(TN) * expect_piecewise_linear(xs, U, first.drift, std::sqrt(first.variance));
    }

    void HullWhiteSwaptionEngine::visit(const InterestRateSwap &swap)
    {
        const auto &m = require_model<HullWhiteCurveModel>("HullWhiteSwaptionEngine");
        const SwapValuation v = value_swap(swap, MultiCurve{m.discount(), m.projection()});
        PricingResult out;
        out.npv = v.npv;
        out.diagnostics = "Multi-curve swap, par rate " + std::to_string(v.par_rate * 1e2) + "%";
        res_ = out;
    }

    void HullWhiteSwaptionEngine::visit(const Swaption &swaption)
    {
        const auto &m = require_model<HullWhiteCurveModel>("HullWhiteSwaptionEngine");
        PricingResult out;
        out.npv = hull_white_european_swaption(swaption, m);
        out.diagnostics = "Hull-White European swaption, closed form (Jamshidian region integration)";
        res_ = out;
    }

    void HullWhiteSwaptionEngine::visit(const BermudanSwaption &bermudan)
    {
        const auto &m = require_model<HullWhiteCurveModel>("HullWhiteSwaptionEngine");
        PricingResult out;
        out.npv = hull_white_bermudan_swaption(bermudan, m);
        out.diagnostics = "Hull-White Bermudan swaption, Gaussian lattice in x (terminal measure)";
        res_ = out;
    }

} // namespace quantModeling
