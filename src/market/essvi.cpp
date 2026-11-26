#include "quantModeling/market/essvi.hpp"
#include "essvi_detail.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace quantModeling
{

    namespace
    {
        /// sqrt(psi^2 k^2 + 2 rho psi theta k + theta^2), never below theta sqrt(1 - rho^2) > 0.
        Real root(Real k, const ESSVIParams &p) noexcept
        {
            const Real x = p.psi * k;
            return std::sqrt(x * x + 2.0 * p.rho * p.theta * x + p.theta * p.theta);
        }
    } // namespace

    Real essvi_total_variance(Real k, const ESSVIParams &p) noexcept
    {
        return 0.5 * (p.theta + p.rho * p.psi * k + root(k, p));
    }

    Real essvi_total_variance_dk(Real k, const ESSVIParams &p) noexcept
    {
        const Real R = root(k, p);
        return 0.5 * (p.rho * p.psi + (p.psi * p.psi * k + p.rho * p.psi * p.theta) / R);
    }

    Real essvi_total_variance_dk2(Real k, const ESSVIParams &p) noexcept
    {
        // d/dk of (psi^2 k + rho psi theta) / R: (psi^2 R^2 - (psi^2 k + rho psi theta)^2) / R^3
        // = psi^2 theta^2 (1 - rho^2) / R^3.
        const Real R = root(k, p);
        return 0.5 * p.psi * p.psi * p.theta * p.theta * (1.0 - p.rho * p.rho) / (R * R * R);
    }

    SVIParams essvi_to_svi(const ESSVIParams &p) noexcept
    {
        SVIParams s;
        s.b = 0.5 * p.psi;
        s.rho = p.rho;
        s.m = -p.rho * p.theta / p.psi;
        s.sigma = p.theta * std::sqrt(1.0 - p.rho * p.rho) / p.psi;
        s.a = 0.5 * p.theta * (1.0 - p.rho * p.rho);
        return s;
    }

    namespace
    {
        using essvi_detail::kStrict;

        /// The largest psi the butterfly bounds allow at (theta, rho).
        Real psi_butterfly_max(Real theta, Real rho) noexcept
        {
            const Real s = 1.0 + std::fabs(rho);
            return std::min(4.0 / s * kStrict, 2.0 * std::sqrt(theta / s));
        }

        /// The largest psi the calendar conditions allow after `before` at
        /// theta: phi = psi / theta non-increasing (Pasquazzi's sufficient
        /// branch, see the header).
        Real psi_calendar_max(const ESSVIParams &before, Real theta) noexcept
        {
            return before.psi * theta / before.theta;
        }

        /// The smallest psi the calendar conditions allow after `before`.
        Real psi_calendar_min(const ESSVIParams &before, Real rho) noexcept
        {
            return std::max(before.psi * (1.0 - before.rho) / (1.0 - rho), before.psi * (1.0 + before.rho) / (1.0 + rho));
        }
    } // namespace

    bool essvi_butterfly_free(const ESSVIParams &p) noexcept
    {
        const Real s = 1.0 + std::fabs(p.rho);
        return p.theta > 0.0 && p.psi > 0.0 && std::fabs(p.rho) < 1.0 && p.psi * s < 4.0 &&
               p.psi * p.psi * s <= 4.0 * p.theta * (1.0 + 1e-12);
    }

    bool essvi_calendar_free(const ESSVIParams &before, const ESSVIParams &after) noexcept
    {
        const Real tol = 1e-12 * std::max(Real(1.0), after.psi);
        return after.theta >= before.theta && after.psi + tol >= before.psi &&
               std::fabs(after.rho * after.psi - before.rho * before.psi) <= after.psi - before.psi + tol &&
               after.psi * before.theta <= before.psi * after.theta * (1.0 + 1e-12);
    }

    // ── Calibration ─────────────────────────────────────────────────────────

    namespace
    {
        struct Slice
        {
            const std::vector<SVISliceQuote> *quotes;
            Real ttm;

            /// sum w_i (iv(k_i) - market_iv_i)^2
            Real objective(const ESSVIParams &p) const noexcept
            {
                Real sum = 0.0;
                for (const SVISliceQuote &q : *quotes)
                {
                    const Real w = essvi_total_variance(q.log_moneyness, p);
                    const Real r = std::sqrt(std::max(w, Real(0.0)) / ttm) - q.market_iv;
                    sum += q.weight * r * r;
                }
                return sum;
            }
        };

        constexpr Real kGolden = 0.6180339887498949;

        /// Golden-section minimum of f on [lo, hi]: (x, f(x)).
        template <class F>
        std::pair<Real, Real> golden(F f, Real lo, Real hi, int iterations)
        {
            Real a = lo, b = hi;
            Real c = b - kGolden * (b - a), d = a + kGolden * (b - a);
            Real fc = f(c), fd = f(d);
            for (int it = 0; it < iterations; ++it)
            {
                if (fc <= fd)
                {
                    b = d, d = c, fd = fc;
                    c = b - kGolden * (b - a);
                    fc = f(c);
                }
                else
                {
                    a = c, c = d, fc = fd;
                    d = a + kGolden * (b - a);
                    fd = f(d);
                }
            }
            return fc <= fd ? std::make_pair(c, fc) : std::make_pair(d, fd);
        }

        /// Brent's minimisation of f on [lo, hi] (Numerical Recipes' brent):
        /// (x, f(x)).
        template <class F>
        std::pair<Real, Real> brent(F f, Real lo, Real hi, int iterations)
        {
            constexpr Real cgold = 0.3819660112501051, zeps = 1e-18;
            const Real tol = 1e-10;
            Real a = lo, b = hi, x = a + cgold * (b - a), w = x, v = x;
            Real fx = f(x), fw = fx, fv = fx, d = 0.0, e = 0.0;
            for (int it = 0; it < iterations; ++it)
            {
                const Real xm = 0.5 * (a + b), tol1 = tol * std::fabs(x) + zeps, tol2 = 2.0 * tol1;
                if (std::fabs(x - xm) <= (tol2 - 0.5 * (b - a)))
                    break;
                bool golden_step = true;
                if (std::fabs(e) > tol1)
                {
                    Real r = (x - w) * (fx - fv), q = (x - v) * (fx - fw), p = (x - v) * q - (x - w) * r;
                    q = 2.0 * (q - r);
                    if (q > 0.0)
                        p = -p;
                    q = std::fabs(q);
                    const Real etemp = e;
                    e = d;
                    if (!(std::fabs(p) >= std::fabs(0.5 * q * etemp) || p <= q * (a - x) || p >= q * (b - x)))
                    {
                        d = p / q;
                        const Real u = x + d;
                        if (u - a < tol2 || b - u < tol2)
                            d = xm >= x ? tol1 : -tol1;
                        golden_step = false;
                    }
                }
                if (golden_step)
                {
                    e = (x >= xm ? a : b) - x;
                    d = cgold * e;
                }
                const Real u = std::fabs(d) >= tol1 ? x + d : x + (d >= 0.0 ? tol1 : -tol1);
                const Real fu = f(u);
                if (fu <= fx)
                {
                    (u >= x ? a : b) = x;
                    v = w, w = x, x = u;
                    fv = fw, fw = fx, fx = fu;
                }
                else
                {
                    (u < x ? a : b) = u;
                    if (fu <= fw || w == x)
                    {
                        v = w, w = u;
                        fv = fw, fw = fu;
                    }
                    else if (fu <= fv || v == x || v == w)
                    {
                        v = u;
                        fv = fu;
                    }
                }
            }
            return {x, fx};
        }

    } // namespace

    namespace essvi_detail
    {
        std::vector<std::size_t> atm_quotes(const std::vector<SVISliceQuote> &quotes)
        {
            std::vector<std::size_t> idx(quotes.size());
            for (std::size_t i = 0; i < idx.size(); ++i)
                idx[i] = i;
            // stable, so that a tie in |k| is broken the same way every time
            std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b)
                             { return std::fabs(quotes[a].log_moneyness) < std::fabs(quotes[b].log_moneyness); });
            idx.resize(std::min<std::size_t>(idx.size(), 7));
            return idx;
        }

        Real atm_total_variance(const std::vector<SVISliceQuote> &quotes, Real ttm, Real *std_error)
        {
            std::vector<const SVISliceQuote *> near;
            for (const std::size_t i : atm_quotes(quotes))
                near.push_back(&quotes[i]);
            // normal equations of w = c0 + c1 k + c2 k^2 (c2 dropped with fewer than four points)
            const int n = near.size() >= 4 ? 3 : 2;
            Real A[3][3] = {}, y[3] = {};
            for (const SVISliceQuote *q : near)
            {
                const Real k = q->log_moneyness, w = q->market_iv * q->market_iv * ttm;
                const Real basis[3] = {1.0, k, k * k};
                for (int i = 0; i < n; ++i)
                {
                    y[i] += q->weight * basis[i] * w;
                    for (int j = 0; j < n; ++j)
                        A[i][j] += q->weight * basis[i] * basis[j];
                }
            }
            // Gaussian elimination
            for (int i = 0; i < n; ++i)
            {
                int piv = i;
                for (int r = i + 1; r < n; ++r)
                    if (std::fabs(A[r][i]) > std::fabs(A[piv][i]))
                        piv = r;
                std::swap(A[i], A[piv]);
                std::swap(y[i], y[piv]);
                if (std::fabs(A[i][i]) < 1e-300)
                    return near.front()->market_iv * near.front()->market_iv * ttm;
                for (int r = i + 1; r < n; ++r)
                {
                    const Real f = A[r][i] / A[i][i];
                    for (int c = i; c < n; ++c)
                        A[r][c] -= f * A[i][c];
                    y[r] -= f * y[i];
                }
            }
            Real c[3] = {};
            for (int i = n - 1; i >= 0; --i)
            {
                Real s = y[i];
                for (int j = i + 1; j < n; ++j)
                    s -= A[i][j] * c[j];
                c[i] = s / A[i][i];
            }
            if (std_error != nullptr)
            {
                // the residuals' weighted spread over the points' count: the
                // estimate's noise, floored so that no maturity weighs alone
                Real ss = 0.0, ws = 0.0;
                for (const SVISliceQuote *q : near)
                {
                    const Real k = q->log_moneyness;
                    const Real r = q->market_iv * q->market_iv * ttm - (c[0] + c[1] * k + (n == 3 ? c[2] * k * k : 0.0));
                    ss += q->weight * r * r;
                    ws += q->weight;
                }
                const Real spread = ws > 0.0 ? std::sqrt(ss / ws) : 0.0;
                *std_error = std::max(spread / std::sqrt(static_cast<Real>(near.size())), 1e-3 * std::fabs(c[0]) + 1e-8);
            }
            return c[0] > 0.0 ? c[0] : near.front()->market_iv * near.front()->market_iv * ttm;
        }
    } // namespace essvi_detail

    namespace
    {
        using essvi_detail::atm_total_variance;

        /// The best (theta, psi) at `rho` under the bounds: the objective, and
        /// the parameters through `out`. +inf if nothing is feasible.
        Real best_at_rho(const Slice &s, Real rho, Real theta_lo, Real theta_hi, const ESSVIParams *before,
                         const ESSVICalibrationSettings &st, ESSVIParams &out)
        {
            const Real inf = std::numeric_limits<Real>::infinity();
            const Real psi_lo = before ? psi_calendar_min(*before, rho) : 1e-8;
            // The psi bound grows with theta: below the theta where it meets
            // psi_lo, nothing is feasible.
            const Real s1 = 1.0 + std::fabs(rho);
            if (psi_lo >= 4.0 / s1 * kStrict)
                return inf;
            const bool fixed = theta_hi == theta_lo;
            // Both upper bounds on psi grow with theta: the butterfly's as
            // sqrt(theta), phi's linearly. Below the theta where either meets
            // psi_lo, nothing is feasible.
            Real theta_min = psi_lo * psi_lo * s1 / 4.0;
            if (before)
                theta_min = std::max(theta_min, psi_lo * before->theta / before->psi);
            if (fixed)
            {
                if (theta_min > theta_lo * (1.0 + 1e-12))
                    return inf;
            }
            else
            {
                theta_lo = std::max(theta_lo, theta_min);
                if (!(theta_hi > theta_lo))
                    return inf;
            }
            ESSVIParams best{theta_lo, rho, psi_lo};
            Real best_f = inf;
            const auto at_theta = [&](Real theta)
            {
                Real psi_hi = before ? std::min(psi_butterfly_max(theta, rho), psi_calendar_max(*before, theta))
                                     : psi_butterfly_max(theta, rho);
                if (!(psi_hi >= psi_lo * (1.0 - 1e-12)))
                    return inf;
                psi_hi = std::max(psi_hi, psi_lo);
                const auto [psi, f] = brent([&](Real x)
                                            { return s.objective({theta, rho, x}); }, psi_lo, psi_hi,
                                            st.inner_iterations);
                if (f < best_f)
                {
                    best_f = f;
                    best = {theta, rho, psi};
                }
                return f;
            };
            if (fixed)
                at_theta(theta_lo);
            else
                golden(at_theta, theta_lo, theta_hi, st.inner_iterations);
            out = best;
            return best_f;
        }

        ESSVISliceFit fit_slice(const Slice &s, const ESSVIParams *before, const ESSVICalibrationSettings &st,
                                Real theta_fixed = 0.0)
        {
            ESSVISliceFit fit;
            fit.ttm = s.ttm;
            fit.n_quotes = s.quotes->size();
            if (fit.n_quotes < st.min_quotes)
                return fit;
            const Real theta_atm = atm_total_variance(*s.quotes, s.ttm);
            Real theta_lo = before ? before->theta * (1.0 + 1e-12) : theta_atm * (1.0 - st.theta_span);
            Real theta_hi = std::max(theta_atm * (1.0 + st.theta_span), theta_lo * (1.0 + st.theta_span));
            if (theta_fixed > 0.0) // on the term structure (smooth_theta)
            {
                if (before && !(theta_fixed > before->theta))
                    return fit;
                theta_lo = theta_fixed, theta_hi = theta_fixed;
            }
            const Real inf = std::numeric_limits<Real>::infinity();

            Real best_f = inf, best_rho = 0.0;
            ESSVIParams best;
            const auto at = [&](Real rho)
            {
                ESSVIParams p;
                const Real f = best_at_rho(s, rho, theta_lo, theta_hi, before, st, p);
                if (f < best_f)
                    best_f = f, best_rho = rho, best = p;
                return f;
            };
            for (int i = 0; i < st.rho_grid; ++i)
                at(-st.rho_max + 2.0 * st.rho_max * (i + 0.5) / st.rho_grid);
            if (!std::isfinite(best_f))
                return fit; // empty feasible set
            const Real step = 2.0 * st.rho_max / st.rho_grid;
            golden(at, std::max(-st.rho_max, best_rho - step), std::min(st.rho_max, best_rho + step), st.refine_iterations);

            fit.params = best;
            fit.feasible = true;
            Real wsum = 0.0, sq = 0.0, worst = 0.0;
            for (const SVISliceQuote &q : *s.quotes)
            {
                const Real r = std::sqrt(essvi_total_variance(q.log_moneyness, best) / s.ttm) - q.market_iv;
                wsum += q.weight;
                sq += q.weight * r * r;
                worst = std::max(worst, std::fabs(r));
            }
            fit.rmse = wsum > 0.0 ? std::sqrt(sq / wsum) : 0.0;
            fit.worst_residual = worst;
            return fit;
        }
    } // namespace

    std::vector<ESSVISliceFit> calibrate_essvi_slices(const std::vector<std::vector<SVISliceQuote>> &quotes,
                                                      const std::vector<Real> &ttms,
                                                      const ESSVICalibrationSettings &settings)
    {
        if (quotes.size() != ttms.size())
            throw InvalidInput("calibrate_essvi_slices: one quote list per maturity");
        for (std::size_t i = 1; i < ttms.size(); ++i)
            if (!(ttms[i] > ttms[i - 1]))
                throw InvalidInput("calibrate_essvi_slices: maturities must increase");
        std::vector<Real> theta(ttms.size(), 0.0);
        if (settings.smooth_theta)
        {
            std::vector<std::size_t> used;
            std::vector<Real> T, est, weight;
            for (std::size_t i = 0; i < ttms.size(); ++i)
                if (quotes[i].size() >= settings.min_quotes)
                {
                    Real se = 0.0;
                    const Real th = atm_total_variance(quotes[i], ttms[i], &se);
                    used.push_back(i);
                    T.push_back(ttms[i]);
                    est.push_back(th);
                    weight.push_back(1.0 / (se * se));
                }
            const std::vector<Real> curve = monotone_theta(T, est, weight, settings.min_forward_vol * settings.min_forward_vol,
                                                           settings.forward_smoothing);
            for (std::size_t j = 0; j < used.size(); ++j)
                theta[used[j]] = curve[j];
        }
        std::vector<ESSVISliceFit> out;
        out.reserve(ttms.size());
        const ESSVIParams *before = nullptr;
        for (std::size_t i = 0; i < ttms.size(); ++i)
        {
            out.push_back(fit_slice(Slice{&quotes[i], ttms[i]}, before, settings, theta[i]));
            if (out.back().feasible)
                before = &out.back().params;
        }
        return out;
    }

    std::vector<Real> monotone_theta(const std::vector<Real> &T, const std::vector<Real> &theta_hat,
                                     const std::vector<Real> &weight, Real v_min, Real smoothing)
    {
        const std::size_t n = T.size();
        if (n == 0)
            return {};
        std::vector<Real> d(n), f(n), w(n), e(n);
        Real mean_w = 0.0, mean_d = 0.0;
        for (std::size_t j = 0; j < n; ++j)
        {
            d[j] = T[j] - (j ? T[j - 1] : 0.0);
            w[j] = std::max(weight[j], Real(1e-300));
            mean_w += w[j] / n;
            mean_d += d[j] / n;
            // start from the data's forward variances, floored
            f[j] = std::max(v_min, (theta_hat[j] - (j ? theta_hat[j - 1] : 0.0)) / d[j]);
        }
        const Real lambda = smoothing * mean_w * mean_d * mean_d * mean_d;
        const auto mid = [&](std::size_t j)
        { return 0.5 * (d[j] + d[j - 1]); };
        // residuals e_i = theta_i - theta_hat_i
        Real acc = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            acc += f[i] * d[i];
            e[i] = acc - theta_hat[i];
        }
        for (int sweep = 0; sweep < 5000; ++sweep)
        {
            Real change = 0.0;
            for (std::size_t j = 0; j < n; ++j)
            {
                // J as a function of f_j: a parabola; its exact minimiser, floored
                Real g = 0.0, H = 0.0;
                for (std::size_t i = j; i < n; ++i)
                {
                    g += 2.0 * w[i] * e[i] * d[j];
                    H += 2.0 * w[i] * d[j] * d[j];
                }
                if (j > 0)
                {
                    g += 2.0 * lambda * (f[j] - f[j - 1]) / mid(j);
                    H += 2.0 * lambda / mid(j);
                }
                if (j + 1 < n)
                {
                    g -= 2.0 * lambda * (f[j + 1] - f[j]) / mid(j + 1);
                    H += 2.0 * lambda / mid(j + 1);
                }
                const Real next = std::max(v_min, f[j] - g / H);
                const Real delta = next - f[j];
                if (delta != 0.0)
                {
                    for (std::size_t i = j; i < n; ++i)
                        e[i] += delta * d[j];
                    f[j] = next;
                    change = std::max(change, std::fabs(delta) / std::max(f[j], v_min));
                }
            }
            if (change < 1e-13)
                break;
        }
        std::vector<Real> out(n);
        acc = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            acc += f[i] * d[i];
            out[i] = (i && acc <= out[i - 1]) ? out[i - 1] * (1.0 + 1e-9) + 1e-12 : acc;
        }
        return out;
    }

    std::vector<bool> essvi_surface_pillars(const std::vector<ESSVISliceFit> &fits, Real min_relative_gap)
    {
        std::vector<bool> pillar(fits.size(), false);
        std::vector<std::size_t> kept;
        std::size_t longest = fits.size();
        for (std::size_t s = 0; s < fits.size(); ++s)
        {
            if (!fits[s].feasible)
                continue;
            if (longest == fits.size() || fits[s].ttm > fits[longest].ttm)
                longest = s;
            if (kept.empty() || fits[s].ttm >= (1.0 + min_relative_gap) * fits[kept.back()].ttm)
                kept.push_back(s);
        }
        if (kept.empty())
            return pillar;
        if (kept.back() != longest)
        {
            if (kept.size() > 1)
                kept.back() = longest;
            else
                kept.push_back(longest);
        }
        for (const std::size_t s : kept)
            pillar[s] = true;
        return pillar;
    }

    // ── The surface ─────────────────────────────────────────────────────────

    ESSVISurface::ESSVISurface(std::vector<ESSVISliceFit> fits)
    {
        for (ESSVISliceFit &f : fits)
            if (f.feasible)
                slices_.push_back(std::move(f));
        if (slices_.empty())
            throw InvalidInput("ESSVISurface: no feasible slice");
        std::sort(slices_.begin(), slices_.end(), [](const ESSVISliceFit &a, const ESSVISliceFit &b)
                  { return a.ttm < b.ttm; });
    }

    ESSVISurface::Bracket ESSVISurface::bracket(Real T) const noexcept
    {
        const Real clamped = std::clamp(T, Real(0.0), ttm_max());
        if (clamped <= ttm_min())
            return {kZeroSlice, 0, clamped / ttm_min(), ttm_min()};
        std::size_t i = 0;
        while (i + 2 < slices_.size() && slices_[i + 1].ttm < clamped)
            ++i;
        const Real T0 = slices_[i].ttm, T1 = slices_[i + 1].ttm;
        return {i, i + 1, (clamped - T0) / (T1 - T0), T1 - T0};
    }

    namespace
    {
        /// The long end's theta slope: the last segment's (theta_N / T_N with one slice).
        Real long_theta_slope(const std::vector<ESSVISliceFit> &s) noexcept
        {
            const std::size_t n = s.size();
            if (n == 1)
                return s[0].params.theta / s[0].ttm;
            return (s[n - 1].params.theta - s[n - 2].params.theta) / (s[n - 1].ttm - s[n - 2].ttm);
        }
    } // namespace

    ESSVIParams ESSVISurface::params(Real T) const noexcept
    {
        if (T > ttm_max())
        {
            ESSVIParams p = slices_.back().params;
            p.theta += long_theta_slope(slices_) * (T - ttm_max());
            return p;
        }
        const Bracket br = bracket(T);
        const ESSVIParams &up = slices_[br.upper].params;
        if (br.lower == kZeroSlice) // theta and psi scale with T / T_1, rho held (Corbetta et al. §5.2)
            return {br.lambda * up.theta, up.rho, br.lambda * up.psi};
        const ESSVIParams &lo = slices_[br.lower].params;
        const Real l = br.lambda;
        const Real theta = (1.0 - l) * lo.theta + l * up.theta;
        const Real psi = (1.0 - l) * lo.psi + l * up.psi;
        const Real rho_psi = (1.0 - l) * lo.rho * lo.psi + l * up.rho * up.psi;
        return {theta, rho_psi / psi, psi};
    }

    void ESSVISurface::params_dT(Real T, Real &theta_T, Real &psi_T, Real &rho_psi_T) const noexcept
    {
        if (T > ttm_max())
        {
            theta_T = long_theta_slope(slices_), psi_T = 0.0, rho_psi_T = 0.0;
            return;
        }
        const Bracket br = bracket(T);
        const ESSVIParams &up = slices_[br.upper].params;
        if (br.lower == kZeroSlice)
        {
            theta_T = up.theta / br.width, psi_T = up.psi / br.width, rho_psi_T = up.rho * up.psi / br.width;
            return;
        }
        const ESSVIParams &lo = slices_[br.lower].params;
        theta_T = (up.theta - lo.theta) / br.width;
        psi_T = (up.psi - lo.psi) / br.width;
        rho_psi_T = (up.rho * up.psi - lo.rho * lo.psi) / br.width;
    }

    Real ESSVISurface::total_variance(Real k, Real T) const noexcept
    {
        return essvi_total_variance(k, params(T));
    }

    Real ESSVISurface::total_variance_dk(Real k, Real T) const noexcept
    {
        return essvi_total_variance_dk(k, params(T));
    }

    Real ESSVISurface::total_variance_dk2(Real k, Real T) const noexcept
    {
        return essvi_total_variance_dk2(k, params(T));
    }

    Real ESSVISurface::total_variance_dT(Real k, Real T) const noexcept
    {
        // w = (theta + p k + R) / 2, R^2 = psi^2 k^2 + 2 p theta k + theta^2, p = rho psi:
        // dw/dT = (theta_T + p_T k + (psi psi_T k^2 + (p_T theta + p theta_T) k + theta theta_T) / R) / 2.
        const ESSVIParams q = params(T);
        Real theta_T, psi_T, p_T;
        params_dT(T, theta_T, psi_T, p_T);
        const Real p = q.rho * q.psi;
        const Real R = std::sqrt(q.psi * q.psi * k * k + 2.0 * p * q.theta * k + q.theta * q.theta);
        return 0.5 * (theta_T + p_T * k + (q.psi * psi_T * k * k + (p_T * q.theta + p * theta_T) * k + q.theta * theta_T) / R);
    }

    Real ESSVISurface::implied_vol(Real k, Real T) const noexcept
    {
        return std::sqrt(std::max(total_variance(k, T), Real(0.0)) / T);
    }

} // namespace quantModeling
