#include "quantModeling/market/slv_calibration.hpp"

#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{
    namespace
    {
        /// Piecewise-linear interpolation of one T-column of the local-vol
        /// (or leverage) grid, flat-extrapolated at the ends -- same
        /// bracketing convention as GridLocalVol::value() and
        /// mc::local_vol_bilinear() (models/equity/path_steps.hpp), collapsed to one dimension
        /// because a column is exactly what this calibration ever looks up
        /// (K at a fixed, already-selected T index).
        Real interp_1d(const std::vector<Real> &K_grid, const std::vector<Real> &column, Real S)
        {
            const int n = static_cast<int>(K_grid.size());
            S = std::clamp(S, K_grid.front(), K_grid.back());
            auto it = std::lower_bound(K_grid.begin(), K_grid.end(), S);
            int i1 = static_cast<int>(it - K_grid.begin());
            if (i1 >= n)
                i1 = n - 1;
            int i0 = (i1 > 0) ? i1 - 1 : 0;
            if (i0 == i1)
            {
                if (i1 > 0)
                    i0 = i1 - 1;
                else
                    i1 = 1;
            }
            const Real K0 = K_grid[static_cast<std::size_t>(i0)];
            const Real K1 = K_grid[static_cast<std::size_t>(i1)];
            const Real w = (K1 - K0 > 1e-12) ? (S - K0) / (K1 - K0) : 0.0;
            return (1.0 - w) * column[static_cast<std::size_t>(i0)] +
                   w * column[static_cast<std::size_t>(i1)];
        }

        /// Index of the K_grid point nearest S -- the bucket a particle at
        /// S is assigned to (a Voronoi cell around each grid point).
        std::size_t nearest_bucket(const std::vector<Real> &K_grid, Real S)
        {
            auto it = std::lower_bound(K_grid.begin(), K_grid.end(), S);
            if (it == K_grid.begin())
                return 0;
            if (it == K_grid.end())
                return K_grid.size() - 1;
            const std::size_t hi = static_cast<std::size_t>(it - K_grid.begin());
            const std::size_t lo = hi - 1;
            return (S - K_grid[lo] <= K_grid[hi] - S) ? lo : hi;
        }

        /// E[v | S = K_i] at every strike: a quartic-kernel average of the
        /// particles' floored variance, bandwidth h (Guyon & Henry-Labordere
        /// 2012). The particles are sorted by spot, so each strike reads only
        /// those within h of it. A strike no particle reaches takes the
        /// nearest estimate; all NaN when no particle reaches any strike.
        void kernel_conditional_variance(const std::vector<Real> &K_grid, const std::vector<Real> &S,
                                         const std::vector<Real> &v, Real variance_floor, Real h,
                                         std::vector<std::size_t> &order, std::vector<Real> &out)
        {
            const std::size_t N = S.size();
            const std::size_t nK = K_grid.size();
            order.resize(N);
            for (std::size_t p = 0; p < N; ++p)
                order[p] = p;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
                      { return S[a] < S[b]; });
            std::size_t lo = 0;
            for (std::size_t i = 0; i < nK; ++i)
            {
                const Real K = K_grid[i];
                while (lo < N && S[order[lo]] < K - h)
                    ++lo;
                Real w_sum = 0.0, wv_sum = 0.0;
                for (std::size_t k = lo; k < N && S[order[k]] <= K + h; ++k)
                {
                    const std::size_t p = order[k];
                    const Real x = (S[p] - K) / h;
                    const Real w = (1.0 - x * x) * (1.0 - x * x);
                    w_sum += w;
                    wv_sum += w * std::max(v[p], variance_floor);
                }
                out[i] = w_sum > 0.0 ? wv_sum / w_sum : std::numeric_limits<Real>::quiet_NaN();
            }
            std::size_t first = 0;
            while (first < nK && std::isnan(out[first]))
                ++first;
            if (first == nK)
                return;
            for (std::size_t i = 0; i < first; ++i)
                out[i] = out[first];
            for (std::size_t i = first + 1; i < nK; ++i)
                if (std::isnan(out[i]))
                    out[i] = out[i - 1];
        }
    } // namespace

    SLVLeverageGrid calibrate_slv_leverage(
        Real s0, Real r, Real q, const HestonParams &heston,
        const std::vector<Real> &K_grid, const std::vector<Real> &T_grid,
        const std::vector<Real> &sigma_loc,
        const SLVCalibrationSettings &settings)
    {
        if (K_grid.size() < 2 || T_grid.size() < 2)
            throw InvalidInput("calibrate_slv_leverage: K_grid and T_grid need at least 2 points each");
        if (sigma_loc.size() != K_grid.size() * T_grid.size())
            throw InvalidInput("calibrate_slv_leverage: sigma_loc size must equal K_grid.size() * T_grid.size()");
        if (settings.n_particles == 0)
            throw InvalidInput("calibrate_slv_leverage: need at least one particle");

        const std::size_t nK = K_grid.size();
        const std::size_t nT = T_grid.size();
        const std::size_t N = settings.n_particles;

        // The leverage is calibrated -- and returned -- at every substep of
        // max_dt, not only at the surface's maturities: SLV holds a column
        // over the interval after it (mc::leverage_staircase), and E[v | S]
        // moves within a surface interval of a quarter or half a year, so a
        // leverage held that long carried the wrong variance (issue #227: a
        // skewed surface's one-year 90 put 4.7 % off; with a column every
        // 0.02 year, 0.08 %). The target sigma_loc at a substep is the
        // surface's, linear in time between its columns as LocalVolSimModel
        // reads it (column 0 before the first maturity).
        const auto sigma_at = [&](std::size_t i, Time t)
        {
            if (t <= T_grid.front())
                return sigma_loc[i * nT];
            if (t >= T_grid.back())
                return sigma_loc[i * nT + nT - 1];
            std::size_t j = 1;
            while (T_grid[j] < t)
                ++j;
            const Real w = (t - T_grid[j - 1]) / (T_grid[j] - T_grid[j - 1]);
            return (1.0 - w) * sigma_loc[i * nT + j - 1] + w * sigma_loc[i * nT + j];
        };

        std::vector<Time> times;    // the returned grid's maturities: every substep
        std::vector<Real> leverage; // K-major over `times`, filled column by column below
        std::vector<std::vector<Real>> columns;
        std::vector<Real> prev_column(nK); // leverage at the previous substep

        // Bootstrap: every particle starts exactly at s0, so the only
        // leverage value the very first step ever reads is L(s0, ~0), a
        // single scalar -- there is no distribution to bucket yet.
        std::vector<Real> col0(nK);
        for (std::size_t i = 0; i < nK; ++i)
            col0[i] = sigma_loc[i * nT + 0];
        const Real L0 = interp_1d(K_grid, col0, s0) /
                        std::sqrt(std::max(heston.v0, settings.variance_floor));
        std::fill(prev_column.begin(), prev_column.end(), L0);

        std::vector<Real> S(N, s0), v(N, heston.v0);
        Pcg32 rng = RngFactory(settings.seed).make(0);
        NormalBoxMuller bm_spot, bm_vol;

        std::vector<std::size_t> order;      // particles sorted by spot, for the kernel
        std::vector<Real> kernel_v(nK, 0.0); // E[v | S = K_i] by kernel regression
        std::size_t clamped_particles = 0;
        Time t_prev = 0.0;
        for (std::size_t j = 0; j < nT; ++j)
        {
            const Time t_target = T_grid[j];
            const Time interval = t_target - t_prev;
            const std::size_t n_sub = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::ceil(interval / settings.max_dt)));
            const Real dt = interval / static_cast<Real>(n_sub);
            const Real sqdt = std::sqrt(std::max(dt, 0.0));

            for (std::size_t sub = 0; sub < n_sub; ++sub)
            {
                for (std::size_t p = 0; p < N; ++p)
                {
                    const Real Lp = interp_1d(K_grid, prev_column, S[p]);
                    const Real v_plus = std::max(v[p], settings.variance_floor);
                    const Real sqrt_v_plus = std::sqrt(v_plus);

                    const double z_spot = bm_spot(rng);
                    const double z_indep = bm_vol(rng);
                    const Real dW_vol = heston.rho * z_spot +
                                        std::sqrt(1.0 - heston.rho * heston.rho) * z_indep;

                    const Real log_return =
                        (r - q - 0.5 * Lp * Lp * v_plus) * dt + Lp * sqrt_v_plus * sqdt * z_spot;
                    S[p] *= std::exp(log_return);
                    v[p] = v[p] + heston.kappa * (heston.theta - v_plus) * dt +
                           heston.xi * sqrt_v_plus * sqdt * dW_vol;
                }
                const Time t_now = sub + 1 == n_sub ? t_target : t_prev + dt * static_cast<Real>(sub + 1);

                // Bucket the particles by nearest strike, and read off each
                // bucket's mean variance as the E[v|S] estimate -- of the
                // variance the spot actually diffuses with, max(v, floor).
                // Full-truncation Euler lets v itself go negative, and
                // averaging the raw v understated E[v|S] wherever the Feller
                // condition fails (issue #113).
                std::vector<Real> v_sum(nK, 0.0);
                std::vector<std::size_t> count(nK, 0);
                for (std::size_t p = 0; p < N; ++p)
                {
                    const std::size_t b = nearest_bucket(K_grid, S[p]);
                    v_sum[b] += std::max(v[p], settings.variance_floor);
                    ++count[b];
                }
                const bool kernel = settings.kernel_scale > 0.0;
                if (kernel)
                {
                    const Real sigma_atm = sigma_at(nearest_bucket(K_grid, s0), t_now);
                    const Real h = settings.kernel_scale * sigma_atm * s0 *
                                   std::sqrt(std::max(t_now, settings.kernel_min_time)) *
                                   std::pow(static_cast<Real>(N), -0.2);
                    kernel_conditional_variance(K_grid, S, v, settings.variance_floor, h, order, kernel_v);
                }

                std::vector<Real> column(nK);
                for (std::size_t i = 0; i < nK; ++i)
                {
                    // An empty bucket (the grid's far tails): the long-run
                    // variance, a defensible order of magnitude where no
                    // particle carries weight. The kernel estimate, when
                    // asked for, replaces the bucket's average.
                    Real mean_v = count[i] > 0 ? v_sum[i] / static_cast<Real>(count[i]) : heston.theta;
                    if (kernel && !std::isnan(kernel_v[i]))
                        mean_v = kernel_v[i];
                    Real L = sigma_at(i, t_now) / std::sqrt(std::max(mean_v, settings.variance_floor));
                    if (L <= settings.leverage_floor || L >= settings.leverage_cap)
                        clamped_particles += count[i];
                    L = std::clamp(L, settings.leverage_floor, settings.leverage_cap);
                    column[i] = L;
                }
                times.push_back(t_now);
                columns.push_back(column);
                prev_column = std::move(column);
            }
            t_prev = t_target;
        }

        const std::size_t nOut = times.size();
        leverage.assign(nK * nOut, 0.0);
        for (std::size_t c = 0; c < nOut; ++c)
            for (std::size_t i = 0; i < nK; ++i)
                leverage[i * nOut + c] = columns[c][i];
        return SLVLeverageGrid{K_grid, times, leverage,
                               static_cast<Real>(clamped_particles) / static_cast<Real>(N * nOut)};
    }

} // namespace quantModeling
