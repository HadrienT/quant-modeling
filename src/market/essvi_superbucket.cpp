// The Dupire superbucket through the eSSVI calibration (blueprint/wp/30-vol-surface.md,
// lot E3): dV/d(each quoted implied vol) of a price whose model risks are
// dV/dsigma_loc on the local-vol grid an eSSVI surface builds.
//
// The calibration is a chain of four maps, run backwards here:
//
//   quotes -> (theta_hat, weight) per maturity      at-the-money estimate
//          -> theta on the smoothed term structure  a QP under f_j >= v_min
//          -> (rho, psi) per slice, in maturity     a least-squares fit under bounds
//             order, under the bounds of the        that move with theta and with
//             slice before                          the slice before
//          -> sigma_loc on the grid                 Dupire, on an AAD tape
//
// Each optimisation is differentiated by the implicit function theorem on its
// active set, exactly: the fits' full Hessians (dupire_superbucket's
// Gauss-Newton one is first order in the residuals, and a slice held on a
// calendar bound far from its quotes has large ones) and the active bounds'
// curvature through their multipliers.

#include "quantModeling/market/superbucket.hpp"

#include "essvi_detail.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/market/essvi.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace quantModeling
{

    namespace
    {
        using aad::Number;

        struct TapeScope
        {
            aad::Tape tape;
            aad::Tape *previous;
            TapeScope()
                : previous(Number::tape) { Number::tape = &tape; }
            ~TapeScope()
            {
                tape.clear();
                Number::tape = previous;
            }
        };

        struct ENum
        {
            Number theta, rho, psi;
        };

        /// Dupire's local variance at (k, T) on tape: ESSVISurface::params and
        /// params_dT, then the analytic w, w_k, w_kk, w_T, on the same doubles.
        Number local_variance_on_tape(const ESSVISurface &surface, const std::vector<ENum> &x, Real k, Real T)
        {
            Number th, ps, pp, thT, psT, ppT;
            const std::size_t n = x.size();
            if (T > surface.ttm_max())
            {
                const Real Tn = surface.slices()[n - 1].ttm;
                const Number slope = n == 1 ? Number(x[0].theta / Tn)
                                            : Number((x[n - 1].theta - x[n - 2].theta) / (Tn - surface.slices()[n - 2].ttm));
                th = x[n - 1].theta + slope * (T - Tn);
                ps = x[n - 1].psi;
                pp = x[n - 1].rho * x[n - 1].psi;
                thT = slope, psT = Number(0.0), ppT = Number(0.0);
            }
            else
            {
                const ESSVISurface::Bracket br = surface.bracket(T);
                const ENum &up = x[br.upper];
                const Real l = br.lambda;
                if (br.lower == ESSVISurface::kZeroSlice)
                {
                    th = l * up.theta, ps = l * up.psi, pp = l * (up.rho * up.psi);
                    thT = up.theta / br.width, psT = up.psi / br.width, ppT = up.rho * up.psi / br.width;
                }
                else
                {
                    const ENum &lo = x[br.lower];
                    th = (1.0 - l) * lo.theta + l * up.theta;
                    ps = (1.0 - l) * lo.psi + l * up.psi;
                    pp = (1.0 - l) * (lo.rho * lo.psi) + l * (up.rho * up.psi);
                    thT = (up.theta - lo.theta) / br.width;
                    psT = (up.psi - lo.psi) / br.width;
                    ppT = (up.rho * up.psi - lo.rho * lo.psi) / br.width;
                }
            }
            const Number R = sqrt(ps * ps * (k * k) + 2.0 * k * (pp * th) + th * th);
            const Number w = 0.5 * (th + pp * k + R);
            const Number wk = 0.5 * (pp + (ps * ps * k + pp * th) / R);
            const Number wkk = 0.5 * th * th * (ps * ps - pp * pp) / (R * R * R);
            const Number wT = 0.5 * (thT + ppT * k + (ps * psT * (k * k) + (ppT * th + pp * thT) * k + th * thT) / R);
            const Number term1 = 1.0 - (k * wk) / (2.0 * w);
            const Number g = term1 * term1 - (wk * wk / 4.0) * (1.0 / w + 0.25) + wkk / 2.0;
            return wT / g;
        }

        /// iv = sqrt(w / T) at one quote, its gradient in x = (rho, psi,
        /// theta) and its Hessian. With w = (lin + R) / 2, lin = theta + rho
        /// psi k, R^2 = Q = psi^2 k^2 + 2 rho psi theta k + theta^2:
        /// R_x = Q_x / 2R, R_xy = Q_xy / 2R - Q_x Q_y / 4R^3, and
        /// iv_xy = w_xy / (2 T iv) - w_x w_y / (4 T^2 iv^3).
        struct IvDerivatives
        {
            Real iv;
            std::array<Real, 3> d;
            std::array<std::array<Real, 3>, 3> dd;
        };

        IvDerivatives iv_derivatives(Real k, Real ttm, const ESSVIParams &p)
        {
            const Real t = p.theta, r = p.rho, s = p.psi;
            const Real Q = s * s * k * k + 2.0 * r * s * t * k + t * t, R = std::sqrt(Q);
            // x = (rho, psi, theta)
            const std::array<Real, 3> lin_x{s * k, r * k, 1.0};
            const std::array<Real, 3> Q_x{2.0 * s * t * k, 2.0 * s * k * k + 2.0 * r * t * k, 2.0 * r * s * k + 2.0 * t};
            const std::array<std::array<Real, 3>, 3> lin_xx{{{0.0, k, 0.0}, {k, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
            const std::array<std::array<Real, 3>, 3> Q_xx{
                {{0.0, 2.0 * t * k, 2.0 * s * k}, {2.0 * t * k, 2.0 * k * k, 2.0 * r * k}, {2.0 * s * k, 2.0 * r * k, 2.0}}};
            const Real w = 0.5 * (t + r * s * k + R);
            IvDerivatives out;
            out.iv = std::sqrt(std::max(w, Real(1e-300)) / ttm);
            std::array<Real, 3> w_x{};
            for (int a = 0; a < 3; ++a)
                w_x[a] = 0.5 * (lin_x[a] + Q_x[a] / (2.0 * R));
            const Real c1 = 1.0 / (2.0 * ttm * out.iv), c2 = 1.0 / (4.0 * ttm * ttm * out.iv * out.iv * out.iv);
            for (int a = 0; a < 3; ++a)
            {
                out.d[a] = c1 * w_x[a];
                for (int b = 0; b < 3; ++b)
                {
                    const Real R_ab = Q_xx[a][b] / (2.0 * R) - Q_x[a] * Q_x[b] / (4.0 * R * R * R);
                    out.dd[a][b] = c1 * 0.5 * (lin_xx[a][b] + R_ab) - c2 * w_x[a] * w_x[b];
                }
            }
            return out;
        }

        /// A bound psi = b(rho; theta, before) and its partials: d/drho, and
        /// d/d(theta, theta_before, rho_before, psi_before).
        struct PsiBound
        {
            Real value;
            Real d_rho;
            std::array<Real, 4> d_u;
            Real d_rho_rho;              ///< the bound's curvature in rho
            std::array<Real, 4> d_rho_u; ///< d^2 b / d rho d u
        };

        /// The psi bounds of essvi.cpp's best_at_rho that bind at the fit.
        std::vector<PsiBound> active_psi_bounds(const ESSVIParams &p, const ESSVIParams *before)
        {
            using essvi_detail::kStrict;
            const Real r = p.rho, t = p.theta, sg = r >= 0.0 ? 1.0 : -1.0, s1 = 1.0 + std::fabs(r);
            std::vector<PsiBound> lower, upper;
            if (before != nullptr)
            {
                const Real pb = before->psi, rb = before->rho, tb = before->theta;
                const Real m1 = 1.0 - r, p1 = 1.0 + r;
                lower.push_back({pb * (1.0 - rb) / m1, pb * (1.0 - rb) / (m1 * m1), {0.0, 0.0, -pb / m1, (1.0 - rb) / m1}, 2.0 * pb * (1.0 - rb) / (m1 * m1 * m1), {0.0, 0.0, -pb / (m1 * m1), (1.0 - rb) / (m1 * m1)}});
                lower.push_back({pb * (1.0 + rb) / p1, -pb * (1.0 + rb) / (p1 * p1), {0.0, 0.0, pb / p1, (1.0 + rb) / p1}, 2.0 * pb * (1.0 + rb) / (p1 * p1 * p1), {0.0, 0.0, -pb / (p1 * p1), -(1.0 + rb) / (p1 * p1)}});
                upper.push_back({pb * t / tb, 0.0, {pb / tb, -pb * t / (tb * tb), 0.0, t / tb}, 0.0, {0.0, 0.0, 0.0, 0.0}});
            }
            upper.push_back({4.0 * kStrict / s1, -4.0 * kStrict * sg / (s1 * s1), {0.0, 0.0, 0.0, 0.0}, 8.0 * kStrict / (s1 * s1 * s1), {0.0, 0.0, 0.0, 0.0}});
            upper.push_back({2.0 * std::sqrt(t / s1), -sg * std::sqrt(t) * std::pow(s1, -1.5), {1.0 / std::sqrt(t * s1), 0.0, 0.0, 0.0}, 1.5 * std::sqrt(t) * std::pow(s1, -2.5), {-sg * 0.5 / std::sqrt(t) * std::pow(s1, -1.5), 0.0, 0.0, 0.0}});
            const Real tol = 1e-7 * p.psi;
            std::vector<PsiBound> active;
            for (const auto *set : {&lower, &upper})
                for (const PsiBound &b : *set)
                    if (std::fabs(p.psi - b.value) <= tol)
                        active.push_back(b);
            return active;
        }

        /// d(rho, psi) / d(u) at one slice's fit, u = (its quotes' ivs,
        /// theta, theta_before, rho_before, psi_before): the implicit function
        /// theorem on the fit's KKT conditions, exact -- the objective's full
        /// Hessian, residual terms included, and the active constraints'
        /// curvature weighted by their multipliers. On the active set,
        /// d(rho, psi) = P du + N dz, N a basis of the directions the
        /// constraints leave free; dz solves the reduced system.
        Eigen::MatrixXd fit_jacobian(const std::vector<SVISliceQuote> &q, Real ttm, const ESSVIParams &p,
                                     const ESSVIParams *before, Real rho_max)
        {
            const auto n = static_cast<Eigen::Index>(q.size());
            const Eigen::Index nu = n + 4, iT = n; // theta's column; the slice before's follow
            // objective sum w_i r_i^2, r_i = iv_i - m_i; y = (rho, psi)
            Eigen::Matrix2d H = Eigen::Matrix2d::Zero();
            Eigen::Vector2d g = Eigen::Vector2d::Zero();
            Eigen::MatrixXd B = Eigen::MatrixXd::Zero(2, nu); // d(gradient in y) / du
            for (Eigen::Index i = 0; i < n; ++i)
            {
                const SVISliceQuote &x = q[static_cast<std::size_t>(i)];
                const IvDerivatives dv = iv_derivatives(x.log_moneyness, ttm, p);
                const Real wr = x.weight, r = dv.iv - x.market_iv;
                for (int a = 0; a < 2; ++a)
                {
                    g(a) += 2.0 * wr * r * dv.d[a];
                    B(a, i) = -2.0 * wr * dv.d[a];
                    B(a, iT) += 2.0 * wr * (dv.d[a] * dv.d[2] + r * dv.dd[a][2]);
                    for (int b = 0; b < 2; ++b)
                        H(a, b) += 2.0 * wr * (dv.d[a] * dv.d[b] + r * dv.dd[a][b]);
                }
            }

            // Active constraints c(y, u) = 0, linearised C_y dy + C_u du = 0,
            // with their second derivatives in (y, y) and (y, u).
            struct Row
            {
                Eigen::RowVector2d cy;
                Eigen::RowVectorXd cu;
                Real c_rho_rho;
                Eigen::RowVectorXd c_rho_u;
            };
            std::vector<Row> rows;
            if (std::fabs(p.rho) >= rho_max * (1.0 - 1e-9))
                rows.push_back({Eigen::RowVector2d(1.0, 0.0), Eigen::RowVectorXd::Zero(nu), 0.0, Eigen::RowVectorXd::Zero(nu)});
            for (const PsiBound &b : active_psi_bounds(p, before))
            {
                // c = psi - b(rho, u)
                Row row{Eigen::RowVector2d(-b.d_rho, 1.0), Eigen::RowVectorXd::Zero(nu), -b.d_rho_rho,
                        Eigen::RowVectorXd::Zero(nu)};
                for (Eigen::Index j = 0; j < 4; ++j)
                {
                    row.cu(iT + j) = -b.d_u[static_cast<std::size_t>(j)];
                    row.c_rho_u(iT + j) = -b.d_rho_u[static_cast<std::size_t>(j)];
                }
                rows.push_back(row);
            }

            Eigen::MatrixXd P = Eigen::MatrixXd::Zero(2, nu), N = Eigen::Matrix2d::Identity();
            Eigen::Matrix2d Lyy = H;
            Eigen::MatrixXd Lyu = B;
            if (!rows.empty())
            {
                const auto na = static_cast<Eigen::Index>(rows.size());
                Eigen::MatrixXd Cy(na, 2), Cu(na, nu);
                for (Eigen::Index a = 0; a < na; ++a)
                {
                    Cy.row(a) = rows[static_cast<std::size_t>(a)].cy;
                    Cu.row(a) = rows[static_cast<std::size_t>(a)].cu;
                }
                // multipliers: g + C_y' mu = 0
                const Eigen::VectorXd mu = -Cy.transpose().completeOrthogonalDecomposition().solve(g);
                for (Eigen::Index a = 0; a < na; ++a)
                {
                    const Row &row = rows[static_cast<std::size_t>(a)];
                    Lyy(0, 0) += mu(a) * row.c_rho_rho;
                    Lyu.row(0) += mu(a) * row.c_rho_u;
                }
                const Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(Cy);
                P = -cod.solve(Cu);
                const Eigen::JacobiSVD<Eigen::MatrixXd> svd(Cy, Eigen::ComputeFullV);
                N = svd.matrixV().rightCols(2 - svd.rank());
            }
            if (N.cols() == 0)
                return P;
            const Eigen::MatrixXd Hr = N.transpose() * Lyy * N;
            const Eigen::MatrixXd K = -Hr.completeOrthogonalDecomposition().solve(N.transpose() * (Lyy * P + Lyu));
            return P + N * K;
        }

        /// Backwards through monotone_theta's QP: (dV/dtheta_hat, dV/dweight)
        /// from dV/dtheta. On the active set A = {f_j = v_min}, the free
        /// forward variances solve Q_FF f_F = 2 (L' W theta_hat)_F - Q_FA v_min,
        /// Q = 2 (L' W L + lambda D), theta = L f.
        void theta_curve_adjoint(const std::vector<Real> &T, const std::vector<Real> &theta_hat,
                                 const std::vector<Real> &weight, const std::vector<Real> &theta, Real v_min,
                                 Real smoothing, const std::vector<Real> &theta_bar, std::vector<Real> &theta_hat_bar,
                                 std::vector<Real> &weight_bar)
        {
            const auto n = static_cast<Eigen::Index>(T.size());
            theta_hat_bar.assign(T.size(), 0.0);
            weight_bar.assign(T.size(), 0.0);
            if (n == 0)
                return;
            Eigen::VectorXd d(n), f(n), w(n), th(n), thh(n), tb(n);
            Real mean_w = 0.0, mean_d = 0.0;
            for (Eigen::Index j = 0; j < n; ++j)
            {
                const auto u = static_cast<std::size_t>(j);
                d(j) = T[u] - (j ? T[u - 1] : 0.0);
                f(j) = (theta[u] - (j ? theta[u - 1] : 0.0)) / d(j);
                w(j) = std::max(weight[u], Real(1e-300));
                th(j) = theta[u], thh(j) = theta_hat[u], tb(j) = theta_bar[u];
                mean_w += w(j) / static_cast<Real>(n);
                mean_d += d(j) / static_cast<Real>(n);
            }
            const Real dlambda_dw = smoothing * mean_d * mean_d * mean_d / static_cast<Real>(n);
            const Real lambda = dlambda_dw * static_cast<Real>(n) * mean_w;
            Eigen::MatrixXd L = Eigen::MatrixXd::Zero(n, n), D = Eigen::MatrixXd::Zero(n, n);
            for (Eigen::Index i = 0; i < n; ++i)
                for (Eigen::Index j = 0; j <= i; ++j)
                    L(i, j) = d(j);
            for (Eigen::Index j = 1; j < n; ++j)
            {
                const Real c = 1.0 / (0.5 * (d(j) + d(j - 1)));
                D(j, j) += c, D(j - 1, j - 1) += c, D(j, j - 1) -= c, D(j - 1, j) -= c;
            }
            const Eigen::MatrixXd Q = 2.0 * (L.transpose() * w.asDiagonal() * L + lambda * D);
            std::vector<Eigen::Index> F;
            for (Eigen::Index j = 0; j < n; ++j)
                if (f(j) > v_min * (1.0 + 1e-10))
                    F.push_back(j);
            if (F.empty())
                return; // every forward variance at its floor: theta does not move
            const auto nf = static_cast<Eigen::Index>(F.size());
            Eigen::MatrixXd QFF(nf, nf), LF(n, nf);
            const Eigen::VectorXd fbar = L.transpose() * tb, Df = D * f;
            Eigen::VectorXd fbF(nf), DfF(nf);
            for (Eigen::Index a = 0; a < nf; ++a)
            {
                fbF(a) = fbar(F[static_cast<std::size_t>(a)]);
                DfF(a) = Df(F[static_cast<std::size_t>(a)]);
                LF.col(a) = L.col(F[static_cast<std::size_t>(a)]);
                for (Eigen::Index b = 0; b < nf; ++b)
                    QFF(a, b) = Q(F[static_cast<std::size_t>(a)], F[static_cast<std::size_t>(b)]);
            }
            const Eigen::VectorXd z = QFF.ldlt().solve(fbF);
            const Eigen::VectorXd Lz = LF * z;
            const Real smooth_term = 2.0 * dlambda_dw * DfF.dot(z);
            for (Eigen::Index i = 0; i < n; ++i)
            {
                theta_hat_bar[static_cast<std::size_t>(i)] = 2.0 * w(i) * Lz(i);
                weight_bar[static_cast<std::size_t>(i)] = 2.0 * (thh(i) - th(i)) * Lz(i) - smooth_term;
            }
        }

        /// Backwards through atm_total_variance: dV/d(market iv) of the
        /// quotes it reads, from dV/dtheta_hat and dV/d(std_error). The fit
        /// is linear in w_i = iv_i^2 T: c = A^-1 X' Wt w.
        void atm_adjoint(const std::vector<SVISliceQuote> &quotes, Real ttm, Real theta_hat_bar, Real se_bar,
                         std::vector<Real> &iv_bar)
        {
            const std::vector<std::size_t> near = essvi_detail::atm_quotes(quotes);
            const auto m = static_cast<Eigen::Index>(near.size());
            const Eigen::Index nb = m >= 4 ? 3 : 2;
            Eigen::MatrixXd X(m, nb);
            Eigen::VectorXd wt(m), w(m);
            for (Eigen::Index i = 0; i < m; ++i)
            {
                const SVISliceQuote &q = quotes[near[static_cast<std::size_t>(i)]];
                const Real k = q.log_moneyness;
                X(i, 0) = 1.0, X(i, 1) = k;
                if (nb == 3)
                    X(i, 2) = k * k;
                wt(i) = q.weight;
                w(i) = q.market_iv * q.market_iv * ttm;
            }
            const Eigen::MatrixXd A = X.transpose() * wt.asDiagonal() * X;
            Eigen::VectorXd w_bar = Eigen::VectorXd::Zero(m);
            if (std::fabs(A.determinant()) < 1e-300)
                w_bar(0) = theta_hat_bar; // the fallback: the nearest quote's w
            else
            {
                const Eigen::MatrixXd S = A.ldlt().solve(X.transpose() * wt.asDiagonal()); // c = S w
                const Eigen::VectorXd c = S * w;
                if (!(c(0) > 0.0))
                    w_bar(0) = theta_hat_bar;
                else
                {
                    const Eigen::VectorXd res = w - X * c;
                    const Real ss = res.dot(wt.asDiagonal() * res), ws = wt.sum();
                    const Real spread = ws > 0.0 ? std::sqrt(ss / ws) : 0.0;
                    const Real sqm = std::sqrt(static_cast<Real>(m));
                    Real c0_bar = theta_hat_bar;
                    if (spread / sqm >= 1e-3 * std::fabs(c(0)) + 1e-8)
                    {
                        // d ss / d w_j = 2 wt_j res_j: the residual is W-orthogonal to the fit
                        if (spread > 0.0)
                            w_bar += se_bar / sqm / (2.0 * spread * ws) * 2.0 * (wt.array() * res.array()).matrix();
                    }
                    else
                        c0_bar += se_bar * 1e-3 * (c(0) >= 0.0 ? 1.0 : -1.0);
                    w_bar += c0_bar * S.row(0).transpose();
                }
            }
            for (Eigen::Index i = 0; i < m; ++i)
            {
                const SVISliceQuote &q = quotes[near[static_cast<std::size_t>(i)]];
                iv_bar[near[static_cast<std::size_t>(i)]] += w_bar(i) * 2.0 * q.market_iv * ttm;
            }
        }
    } // namespace

    SuperbucketResult essvi_superbucket(const std::vector<std::vector<SVISliceQuote>> &quotes,
                                        const std::vector<Real> &ttms, const ESSVICalibrationSettings &settings,
                                        Real spot, Real rate, Real dividend, Real k_min, Real k_max,
                                        std::size_t n_strikes, std::size_t n_maturities,
                                        const std::vector<Real> &dV_dsigma_loc, const DupireFromSVIParams &params)
    {
        if (quotes.size() != ttms.size())
            throw InvalidInput("essvi_superbucket: one quote list per maturity");
        if (dV_dsigma_loc.size() != n_strikes * n_maturities)
            throw InvalidInput("essvi_superbucket: dV_dsigma_loc must have n_strikes x n_maturities entries");
        if (!settings.smooth_theta)
            throw InvalidInput("essvi_superbucket: only the smoothed term structure (smooth_theta) is differentiated");

        // ── The calibration and the grid, as calibrate_vol_surface ran them ──
        const std::vector<ESSVISliceFit> fits = calibrate_essvi_slices(quotes, ttms, settings);
        std::vector<std::size_t> in_surface; // input index of each surface slice
        for (std::size_t s = 0; s < fits.size(); ++s)
            if (fits[s].feasible)
                in_surface.push_back(s);
        const ESSVISurface surface(fits);
        const GridLocalVol grid =
            build_local_vol_grid(surface, spot, rate, dividend, k_min, k_max, n_strikes, n_maturities, params);
        const std::vector<Real> &K = grid.K_grid();
        const std::vector<Real> &Tg = grid.T_grid();
        std::vector<Real> loc_var(n_strikes * n_maturities);
        for (std::size_t i = 0; i < n_strikes; ++i)
            for (std::size_t j = 0; j < n_maturities; ++j)
            {
                const Real k = std::log(K[i] / (spot * std::exp((rate - dividend) * Tg[j])));
                loc_var[i * n_maturities + j] = svi_surface_local_variance(surface, k, Tg[j], params);
            }
        const std::vector<std::size_t> source = nearest_valid_sources(loc_var, n_strikes, n_maturities);

        SuperbucketResult out;
        out.dV_dessvi.assign(quotes.size(), {0.0, 0.0, 0.0});
        const std::size_t ns = in_surface.size();
        std::vector<std::array<Real, 3>> xbar(ns); // dV/d(theta, rho, psi) per surface slice

        // ── Through Dupire, on tape ──────────────────────────────────────────
        {
            TapeScope scope;
            std::vector<ENum> x;
            for (const ESSVISliceFit &f : surface.slices())
                x.push_back({Number(f.params.theta), Number(f.params.rho), Number(f.params.psi)});
            std::vector<Number> lv(loc_var.size());
            std::vector<bool> on(loc_var.size(), false);
            for (std::size_t i = 0; i < n_strikes; ++i)
                for (std::size_t j = 0; j < n_maturities; ++j)
                {
                    const std::size_t idx = i * n_maturities + j;
                    const Real v = loc_var[idx];
                    if (std::isnan(v))
                        continue;
                    on[idx] = true;
                    if (v <= params.min_local_var || v >= params.max_local_var)
                    {
                        lv[idx] = Number(v); // clamped: flat in the parameters
                        continue;
                    }
                    const Real k = std::log(K[i] / (spot * std::exp((rate - dividend) * Tg[j])));
                    lv[idx] = local_variance_on_tape(surface, x, k, Tg[j]);
                }
            Number L(0.0);
            for (std::size_t idx = 0; idx < lv.size(); ++idx)
            {
                if (dV_dsigma_loc[idx] == 0.0)
                    continue;
                const std::size_t src = source[idx];
                if (src == kNoSource || !on[src])
                    continue;
                L += dV_dsigma_loc[idx] * sqrt(lv[src]);
            }
            L.propagate_to_start();
            for (std::size_t s = 0; s < ns; ++s)
            {
                xbar[s] = {x[s].theta.adjoint(), x[s].rho.adjoint(), x[s].psi.adjoint()};
                out.dV_dessvi[in_surface[s]] = xbar[s];
            }
        }

        // ── Through each slice's fit, last to first ──────────────────────────
        std::vector<std::vector<Real>> iv_bar(quotes.size());
        for (std::size_t s = 0; s < quotes.size(); ++s)
            iv_bar[s].assign(quotes[s].size(), 0.0);
        std::vector<Real> theta_bar(quotes.size(), 0.0); // into the term structure
        for (std::size_t s = ns; s-- > 0;)
        {
            const std::size_t at = in_surface[s];
            const ESSVIParams &p = fits[at].params;
            const ESSVIParams *before = s > 0 ? &fits[in_surface[s - 1]].params : nullptr;
            const Eigen::MatrixXd M = fit_jacobian(quotes[at], ttms[at], p, before, settings.rho_max);
            const Eigen::Vector2d ybar(xbar[s][1], xbar[s][2]);
            const Eigen::VectorXd ubar = M.transpose() * ybar;
            const auto n = static_cast<Eigen::Index>(quotes[at].size());
            for (Eigen::Index i = 0; i < n; ++i)
                iv_bar[at][static_cast<std::size_t>(i)] += ubar(i);
            theta_bar[at] += xbar[s][0] + ubar(n);
            if (s > 0)
            {
                xbar[s - 1][0] += ubar(n + 1);
                xbar[s - 1][1] += ubar(n + 2);
                xbar[s - 1][2] += ubar(n + 3);
            }
        }

        // ── Through the term structure and the at-the-money estimates ───────
        std::vector<std::size_t> used;
        std::vector<Real> T, est, weight, se, curve, tb;
        for (std::size_t s = 0; s < quotes.size(); ++s)
            if (quotes[s].size() >= settings.min_quotes)
            {
                Real e = 0.0;
                used.push_back(s);
                T.push_back(ttms[s]);
                est.push_back(essvi_detail::atm_total_variance(quotes[s], ttms[s], &e));
                se.push_back(e);
                weight.push_back(1.0 / (e * e));
                tb.push_back(theta_bar[s]);
            }
        const Real v_min = settings.min_forward_vol * settings.min_forward_vol;
        curve = monotone_theta(T, est, weight, v_min, settings.forward_smoothing);
        std::vector<Real> est_bar, weight_bar;
        theta_curve_adjoint(T, est, weight, curve, v_min, settings.forward_smoothing, tb, est_bar, weight_bar);
        for (std::size_t u = 0; u < used.size(); ++u)
        {
            const Real se_bar = weight_bar[u] * (-2.0 / (se[u] * se[u] * se[u]));
            atm_adjoint(quotes[used[u]], ttms[used[u]], est_bar[u], se_bar, iv_bar[used[u]]);
        }

        for (std::size_t s = 0; s < quotes.size(); ++s)
            for (std::size_t i = 0; i < quotes[s].size(); ++i)
            {
                QuoteVega v;
                v.slice = s;
                v.ttm = ttms[s];
                v.log_moneyness = quotes[s][i].log_moneyness;
                v.strike = spot * std::exp((rate - dividend) * ttms[s] + v.log_moneyness);
                v.implied_vol = quotes[s][i].market_iv;
                v.vega = iv_bar[s][i];
                out.quotes.push_back(v);
            }
        return out;
    }

} // namespace quantModeling
