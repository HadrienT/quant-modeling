#include "quantModeling/market/superbucket.hpp"

#include "quantModeling/aad/number.hpp"
#include "quantModeling/market/svi.hpp"
#include "quantModeling/market/svi_surface.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    namespace
    {
        using aad::Number;

        /// SVI raw parameters as tape numbers.
        struct SviNum
        {
            Number a, b, rho, m, sigma;
        };

        SviNum on_tape(const SVIParams &p)
        {
            return {Number(p.a), Number(p.b), Number(p.rho), Number(p.m), Number(p.sigma)};
        }

        // The formulas of market/svi.cpp, on tape numbers.
        Number w_of(double k, const SviNum &p)
        {
            const Number x = k - p.m;
            const Number D = sqrt(x * x + p.sigma * p.sigma);
            return p.a + p.b * (p.rho * x + D);
        }

        Number wk_of(double k, const SviNum &p)
        {
            const Number x = k - p.m;
            const Number D = sqrt(x * x + p.sigma * p.sigma);
            return p.b * (p.rho + x / D);
        }

        Number wkk_of(double k, const SviNum &p)
        {
            const Number x = k - p.m;
            const Number D = sqrt(x * x + p.sigma * p.sigma);
            return p.b * p.sigma * p.sigma / (D * D * D);
        }

        /// Restores the previous tape pointer and clears ours on scope exit.
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

        std::array<bool, 5> bounds_hit(const std::vector<SVISliceQuote> &quotes, Real ttm,
                                       const SVIParams &p)
        {
            const SVISliceObjective objective(quotes, ttm);
            const std::vector<Real> lo = objective.lower_bounds(), hi = objective.upper_bounds();
            const std::vector<Real> x = SVISliceObjective::pack(p);
            std::array<bool, 5> hit{};
            for (std::size_t j = 0; j < 5; ++j)
            {
                const Real tol = 1e-8 * (1.0 + std::fabs(x[j]));
                hit[j] = std::fabs(x[j] - lo[j]) <= tol || std::fabs(x[j] - hi[j]) <= tol;
            }
            return hit;
        }
    } // namespace

    SuperbucketResult dupire_superbucket(
        const std::vector<SVISliceCalibration> &slices_in,
        const std::vector<std::vector<SVISliceQuote>> &quotes_in,
        Real spot, Real rate, Real dividend, Real k_min, Real k_max,
        std::size_t n_strikes, std::size_t n_maturities,
        const std::vector<Real> &dV_dsigma_loc, const DupireFromSVIParams &params)
    {
        if (slices_in.size() != quotes_in.size())
            throw InvalidInput("dupire_superbucket: one quote list per slice");
        if (dV_dsigma_loc.size() != n_strikes * n_maturities)
            throw InvalidInput("dupire_superbucket: dV_dsigma_loc must have n_strikes x n_maturities entries");

        // Slices in maturity order, as SVISurface sorts them; quotes follow.
        std::vector<std::size_t> order(slices_in.size());
        for (std::size_t s = 0; s < order.size(); ++s)
            order[s] = s;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
                  { return slices_in[a].ttm < slices_in[b].ttm; });
        std::vector<SVISliceCalibration> slices;
        std::vector<std::vector<SVISliceQuote>> quotes;
        for (const std::size_t s : order)
        {
            slices.push_back(slices_in[s]);
            quotes.push_back(quotes_in[s]);
        }
        const std::size_t n_slices = slices.size();

        // ── The double build: the grid, and every decision the tape replays ──
        const SVISurface surface(slices);
        const GridLocalVol grid = build_local_vol_grid(surface, spot, rate, dividend, k_min, k_max,
                                                       n_strikes, n_maturities, params);
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
        out.dV_dsvi.assign(n_slices, {});
        out.at_bound.assign(n_slices, {});

        // ── Step 1: through Dupire, on tape ──────────────────────────────────
        {
            TapeScope scope;
            std::vector<SviNum> theta;
            theta.reserve(n_slices);
            for (const auto &s : slices)
                theta.push_back(on_tape(s.params));

            const Real t_min = slices.front().ttm, t_max = slices.back().ttm;
            std::vector<Number> lv(n_strikes * n_maturities);
            std::vector<bool> on(n_strikes * n_maturities, false);
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
                        lv[idx] = Number(std::clamp(v, params.min_local_var, params.max_local_var));
                        continue; // clamped: flat in the parameters
                    }
                    // SVISurface::bracket, on the same doubles.
                    const Real T = std::clamp(Tg[j], t_min, t_max);
                    std::size_t s = 0;
                    while (s + 2 < n_slices && slices[s + 1].ttm < T)
                        ++s;
                    const Real T0 = slices[s].ttm, T1 = slices[s + 1].ttm;
                    const Real lam = (T - T0) / (T1 - T0);
                    const Real k = std::log(K[i] / (spot * std::exp((rate - dividend) * Tg[j])));

                    const Number w0 = w_of(k, theta[s]), w1 = w_of(k, theta[s + 1]);
                    const Number w = (1.0 - lam) * w0 + lam * w1;
                    const Number wp = (1.0 - lam) * wk_of(k, theta[s]) + lam * wk_of(k, theta[s + 1]);
                    const Number wpp = (1.0 - lam) * wkk_of(k, theta[s]) + lam * wkk_of(k, theta[s + 1]);
                    const Number dwdT = (w1 - w0) / (T1 - T0);
                    const Number term1 = 1.0 - (k * wp) / (2.0 * w);
                    const Number g = term1 * term1 - (wp * wp / 4.0) * (1.0 / w + 0.25) + wpp / 2.0;
                    lv[idx] = dwdT / g;
                }

            Number L(0.0);
            for (std::size_t idx = 0; idx < lv.size(); ++idx)
            {
                if (dV_dsigma_loc[idx] == 0.0)
                    continue;
                const std::size_t src = source[idx];
                if (src == kNoSource || !on[src])
                    continue; // sigma_loc = sqrt(min_local_var): a constant
                const Number sigma = sqrt(lv[src]);
                L += dV_dsigma_loc[idx] * sigma;
            }
            L.propagate_to_start();
            for (std::size_t s = 0; s < n_slices; ++s)
                out.dV_dsvi[s] = {theta[s].a.adjoint(), theta[s].b.adjoint(), theta[s].rho.adjoint(),
                                  theta[s].m.adjoint(), theta[s].sigma.adjoint()};
        }

        // ── Step 2: through each slice's fit, implicit function theorem ─────
        for (std::size_t s = 0; s < n_slices; ++s)
        {
            const std::vector<SVISliceQuote> &q = quotes[s];
            const Real ttm = slices[s].ttm;
            out.at_bound[s] = bounds_hit(q, ttm, slices[s].params);
            std::vector<std::size_t> free;
            for (std::size_t j = 0; j < 5; ++j)
                if (!out.at_bound[s][j])
                    free.push_back(j);

            // J (n_quotes x 5): d iv(k_i; theta) / d theta, by AAD.
            Eigen::MatrixXd J(static_cast<Eigen::Index>(q.size()), 5);
            for (std::size_t i = 0; i < q.size(); ++i)
            {
                TapeScope scope;
                SviNum th = on_tape(slices[s].params);
                const Number w = w_of(q[i].log_moneyness, th);
                Number iv = sqrt(max(w, 0.0) / ttm);
                iv.propagate_to_start();
                const double row[5] = {th.a.adjoint(), th.b.adjoint(), th.rho.adjoint(),
                                       th.m.adjoint(), th.sigma.adjoint()};
                for (Eigen::Index j = 0; j < 5; ++j)
                    J(static_cast<Eigen::Index>(i), j) = row[j];
            }

            Eigen::VectorXd x = Eigen::VectorXd::Zero(5);
            if (!free.empty())
            {
                const auto nf = static_cast<Eigen::Index>(free.size());
                Eigen::MatrixXd A = Eigen::MatrixXd::Zero(nf, nf);
                Eigen::VectorXd g(nf);
                for (Eigen::Index a = 0; a < nf; ++a)
                {
                    g(a) = out.dV_dsvi[s][free[static_cast<std::size_t>(a)]];
                    for (Eigen::Index b = 0; b < nf; ++b)
                        for (std::size_t i = 0; i < q.size(); ++i)
                            A(a, b) += q[i].weight * J(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(free[static_cast<std::size_t>(a)])) *
                                       J(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(free[static_cast<std::size_t>(b)]));
                }
                const Eigen::VectorXd xf = A.completeOrthogonalDecomposition().solve(g);
                for (Eigen::Index a = 0; a < nf; ++a)
                    x(static_cast<Eigen::Index>(free[static_cast<std::size_t>(a)])) = xf(a);
            }

            for (std::size_t i = 0; i < q.size(); ++i)
            {
                QuoteVega v;
                v.slice = s;
                v.ttm = ttm;
                v.log_moneyness = q[i].log_moneyness;
                v.strike = spot * std::exp((rate - dividend) * ttm + q[i].log_moneyness);
                v.implied_vol = q[i].market_iv;
                v.vega = q[i].weight * J.row(static_cast<Eigen::Index>(i)).dot(x);
                out.quotes.push_back(v);
            }
        }
        return out;
    }

} // namespace quantModeling
