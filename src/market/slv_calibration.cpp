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
        /// LocalVolSimModel::local_vol_at(), collapsed to one dimension
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

        std::vector<Real> leverage(nK * nT);
        std::vector<Real> prev_column(nK); // leverage at the previous T_grid slice

        // Bootstrap: every particle starts exactly at s0, so the only
        // leverage value the very first step ever reads is L(s0, ~0), a
        // single scalar -- there is no distribution to bucket yet. sigma_loc
        // is K-major, so column 0 is sigma_loc[i*nT + 0] for i in [0,nK).
        std::vector<Real> col0(nK);
        for (std::size_t i = 0; i < nK; ++i)
            col0[i] = sigma_loc[i * nT + 0];
        const Real L0 = interp_1d(K_grid, col0, s0) /
                        std::sqrt(std::max(heston.v0, settings.variance_floor));
        std::fill(prev_column.begin(), prev_column.end(), L0);

        std::vector<Real> S(N, s0), v(N, heston.v0);
        Pcg32 rng = RngFactory(settings.seed).make(0);
        NormalBoxMuller bm_spot, bm_vol;

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
            }
            t_prev = t_target;

            // Bucket the particles now at T_grid[j] by nearest strike, and
            // read off each bucket's mean variance as the E[v|S] estimate.
            std::vector<Real> v_sum(nK, 0.0);
            std::vector<std::size_t> count(nK, 0);
            for (std::size_t p = 0; p < N; ++p)
            {
                const std::size_t b = nearest_bucket(K_grid, S[p]);
                v_sum[b] += v[p];
                ++count[b];
            }

            std::vector<Real> column(nK);
            for (std::size_t i = 0; i < nK; ++i)
            {
                Real mean_v;
                if (count[i] > 0)
                    mean_v = v_sum[i] / static_cast<Real>(count[i]);
                else
                {
                    // No particle landed exactly in this bucket this
                    // column (common at the grid's far tails): fall back to
                    // the long-run variance rather than guess from
                    // neighbours -- theta is always a defensible order of
                    // magnitude, and an empty bucket already means this
                    // strike/maturity carries little weight in whatever
                    // priced it.
                    mean_v = heston.theta;
                }
                const Real sigma_ij = sigma_loc[i * nT + j];
                Real L = sigma_ij / std::sqrt(std::max(mean_v, settings.variance_floor));
                L = std::clamp(L, settings.leverage_floor, settings.leverage_cap);
                column[i] = L;
                leverage[i * nT + j] = L;
            }
            prev_column = column;
        }

        return SLVLeverageGrid{K_grid, T_grid, leverage};
    }

} // namespace quantModeling
