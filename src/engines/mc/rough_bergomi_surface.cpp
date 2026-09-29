#include "quantModeling/engines/mc/rough_bergomi_surface.hpp"

#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

namespace quantModeling
{

    Real ForwardVarianceCurve::operator()(Time t) const
    {
        const auto it = std::lower_bound(times.begin(), times.end(), t);
        return it == times.end() ? xi.back() : xi[static_cast<std::size_t>(it - times.begin())];
    }

    ForwardVarianceCurve ForwardVarianceCurve::from_total_variance(const std::vector<Time> &maturities,
                                                                   const std::vector<Real> &total_variance,
                                                                   int *floored)
    {
        if (maturities.empty() || maturities.size() != total_variance.size())
            throw InvalidInput("forward variance curve: need one total variance per maturity, and at least one");
        ForwardVarianceCurve c;
        const Real average = total_variance.back() / maturities.back();
        Time t_prev = 0.0;
        Real w_prev = 0.0;
        int n_floored = 0;
        for (std::size_t i = 0; i < maturities.size(); ++i)
        {
            if (!(maturities[i] > t_prev))
                throw InvalidInput("forward variance curve: maturities must be positive and increasing");
            Real fwd = (total_variance[i] - w_prev) / (maturities[i] - t_prev);
            if (!(fwd > 0.1 * average))
            {
                fwd = 0.1 * average;
                ++n_floored;
            }
            c.times.push_back(maturities[i]);
            c.xi.push_back(fwd);
            t_prev = maturities[i];
            w_prev = total_variance[i];
        }
        if (floored)
            *floored = n_floored;
        return c;
    }

    namespace
    {
        /// Running sums of one quote over the antithetic pairs of a block.
        struct Sums
        {
            double y = 0, yy = 0, x = 0, xx = 0, xy = 0;
            void add(double yv, double xv)
            {
                y += yv;
                yy += yv * yv;
                x += xv;
                xx += xv * xv;
                xy += xv * yv;
            }
            void merge(const Sums &o)
            {
                y += o.y;
                yy += o.yy;
                x += o.x;
                xx += o.xx;
                xy += o.xy;
            }
        };

        constexpr int kBlock = 128; // antithetic pairs per block
    } // namespace

    RoughBergomiSurface rough_bergomi_surface(const RoughBergomiParams &params, const ForwardVarianceCurve &xi,
                                              const std::vector<RoughBergomiQuote> &quotes,
                                              const RoughBergomiSurfaceSettings &settings)
    {
        if (quotes.empty())
            return {};
        if (!(params.H > 0.0 && params.H < 0.5) || !(params.eta >= 0.0) || !(std::abs(params.rho) <= 1.0))
            throw InvalidInput("rough Bergomi: need 0 < H < 1/2, eta >= 0 and |rho| <= 1");
        if (settings.steps_per_year < 1 || settings.n_paths < 2)
            throw InvalidInput("rough Bergomi surface: need steps_per_year >= 1 and n_paths >= 2");

        Time T_max = 0.0;
        for (const RoughBergomiQuote &q : quotes)
        {
            if (!(q.ttm > 0.0))
                throw InvalidInput("rough Bergomi surface: maturities must be > 0");
            T_max = std::max(T_max, q.ttm);
        }
        const auto n = static_cast<std::size_t>(std::max(1.0, std::ceil(T_max * settings.steps_per_year - 1e-9)));
        const Real dt = T_max / static_cast<Real>(n);
        const Real alpha = params.H - 0.5;

        // Each maturity is read at its nearest grid date; quotes grouped by it.
        std::map<std::size_t, std::vector<std::size_t>> by_step;
        for (std::size_t i = 0; i < quotes.size(); ++i)
        {
            const auto m = static_cast<std::size_t>(std::clamp<double>(std::round(quotes[i].ttm / dt), 1.0,
                                                                       static_cast<double>(n)));
            by_step[m].push_back(i);
        }
        std::vector<std::vector<std::size_t>> read_at(n + 1);
        for (auto &[m, idx] : by_step)
            read_at[m] = idx;
        std::vector<Real> strike(quotes.size());
        std::vector<bool> call(quotes.size());
        for (std::size_t i = 0; i < quotes.size(); ++i)
        {
            strike[i] = std::exp(quotes[i].k);
            call[i] = quotes[i].k >= 0.0;
        }

        // Hybrid scheme constants (engines/mc/rough_bergomi_hybrid.cpp).
        const Real l11 = std::sqrt(dt);
        const Real l21 = std::pow(dt, alpha + 1.0) / (alpha + 1.0) / l11;
        const Real l22 = std::sqrt(std::max(std::pow(dt, 2.0 * alpha + 1.0) / (2.0 * alpha + 1.0) - l21 * l21, 0.0));
        std::vector<Real> b_alpha(n + 1, 0.0);
        for (std::size_t k = 2; k <= n; ++k)
        {
            const Real kk = static_cast<Real>(k);
            const Real base = (std::pow(kk, alpha + 1.0) - std::pow(kk - 1.0, alpha + 1.0)) / (alpha + 1.0);
            b_alpha[k] = std::pow(std::pow(base, 1.0 / alpha), alpha);
        }
        const Real dt_alpha = std::pow(dt, alpha);
        const Real s2a1 = std::sqrt(2.0 * alpha + 1.0);
        const Real rho_bar = std::sqrt(std::max(1.0 - params.rho * params.rho, 0.0));
        std::vector<Real> xi_at(n + 1), compensator(n + 1);
        for (std::size_t i = 0; i <= n; ++i)
        {
            const Real t = static_cast<Real>(i) * dt;
            xi_at[i] = xi(i == 0 ? 0.0 : t);
            compensator[i] = 0.5 * params.eta * params.eta * std::pow(t, 2.0 * alpha + 1.0);
        }

        const long long pairs = settings.n_paths / 2;
        const long long n_blocks = (pairs + kBlock - 1) / kBlock;
        std::vector<std::vector<Sums>> block_sums(static_cast<std::size_t>(n_blocks),
                                                  std::vector<Sums>(quotes.size()));

        const auto run_block = [&](long long block)
        {
            std::vector<Real> dW1(n + 1), draws(3 * n);
            std::vector<double> X(2 * quotes.size());
            std::vector<Sums> &sums = block_sums[static_cast<std::size_t>(block)];
            const long long first = block * kBlock, last = std::min(pairs, first + kBlock);
            for (long long p = first; p < last; ++p)
            {
                PhiloxGaussianSource src(settings.seed, static_cast<std::uint64_t>(p));
                for (Real &d : draws)
                    d = src.next();
                for (int side = 0; side < 2; ++side)
                {
                    const Real sign = side == 0 ? 1.0 : -1.0;
                    Real v_prev = xi_at[0], log_x = 0.0;
                    for (std::size_t i = 1; i <= n; ++i)
                    {
                        const Real za = sign * draws[3 * (i - 1)], zb = sign * draws[3 * (i - 1) + 1],
                                   z2 = sign * draws[3 * (i - 1) + 2];
                        const Real dw1 = l11 * za, y = l21 * za + l22 * zb;
                        dW1[i] = dw1;
                        const Real dz = params.rho * dw1 + rho_bar * l11 * z2;
                        log_x += std::sqrt(v_prev) * dz - 0.5 * v_prev * dt;
                        Real far = 0.0;
                        for (std::size_t k = 2; k <= i; ++k)
                            far += b_alpha[k] * dW1[i - k + 1];
                        v_prev = xi_at[i] * std::exp(params.eta * s2a1 * (y + dt_alpha * far) - compensator[i]);
                        for (const std::size_t q : read_at[i])
                            X[2 * q + static_cast<std::size_t>(side)] = std::exp(log_x);
                    }
                }
                for (std::size_t q = 0; q < quotes.size(); ++q)
                {
                    const double x0 = X[2 * q], x1 = X[2 * q + 1];
                    const auto pay = [&](double x)
                    { return call[q] ? std::max(x - strike[q], 0.0) : std::max(strike[q] - x, 0.0); };
                    sums[q].add(0.5 * (pay(x0) + pay(x1)), 0.5 * (x0 + x1));
                }
            }
        };

        std::size_t threads = settings.max_threads > 0 ? static_cast<std::size_t>(settings.max_threads)
                                                       : available_cpus();
        threads = std::max<std::size_t>(1, std::min<std::size_t>(threads, static_cast<std::size_t>(n_blocks)));
        std::vector<std::thread> pool;
        for (std::size_t w = 0; w < threads; ++w)
            pool.emplace_back([&, w]
                              {
                                  for (long long b = static_cast<long long>(w); b < n_blocks;
                                       b += static_cast<long long>(threads))
                                      run_block(b); });
        for (std::thread &t : pool)
            t.join();

        // Blocks summed in their own order: the same bits for any thread count.
        std::vector<Sums> total(quotes.size());
        for (const auto &bs : block_sums)
            for (std::size_t q = 0; q < quotes.size(); ++q)
                total[q].merge(bs[q]);

        RoughBergomiSurface out;
        const double N = static_cast<double>(pairs);
        for (std::size_t q = 0; q < quotes.size(); ++q)
        {
            const Sums &s = total[q];
            const double my = s.y / N, mx = s.x / N;
            const double vy = s.yy / N - my * my, vx = s.xx / N - mx * mx, cxy = s.xy / N - mx * my;
            const double beta = vx > 0.0 ? cxy / vx : 0.0;
            // E[X_T] = 1: X is a control variate.
            const double price = my - beta * (mx - 1.0);
            const double var = std::max(vy - (vx > 0.0 ? cxy * cxy / vx : 0.0), 0.0);
            out.otm_prices.push_back(price);
            out.std_errors.push_back(std::sqrt(var / N));
            const double call_price = call[q] ? price : price + (1.0 - strike[q]);
            out.implied_vols.push_back(price > 0.0 ? black76_implied_vol(call_price, 1.0, strike[q], quotes[q].ttm, 1.0)
                                                   : std::numeric_limits<Real>::quiet_NaN());
        }
        return out;
    }

} // namespace quantModeling
