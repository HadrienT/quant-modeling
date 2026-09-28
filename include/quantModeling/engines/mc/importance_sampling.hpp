#ifndef QM_ENGINES_MC_IMPORTANCE_SAMPLING_HPP
#define QM_ENGINES_MC_IMPORTANCE_SAMPLING_HPP

#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"

/**
 * @file importance_sampling.hpp
 * @brief Generic importance sampling for the simulation engine: the drift,
 *        and whether it helps (blueprint/wp/19-gpu.md §2.5).
 *
 * Glasserman, Heidelberger & Shahabuddin (Math. Finance 9, 1999; Glasserman
 * 2004 §4.6.2): sample the path's gaussians z from N(mu, I) instead of
 * N(0, I) and weight the payoff G by the likelihood ratio
 * exp(-mu.x - |mu|^2 / 2) (x = z - mu). The zero-variance change of measure
 * is out of reach; GHS take mu at the mode of G(z) phi(z), i.e. the maximiser
 * of log G(z) - |z|^2 / 2, which puts the samples where the integrand's mass
 * is -- decisive for rare events (a deep out-of-the-money digital, a far
 * knock-in), little elsewhere.
 *
 * Here mu is a constant drift per Brownian factor: draw (d, f) is shifted by
 * theta_f sqrt(t_d - t_{d-1}), W_f(t) gains theta_f t -- the dedicated
 * Black-Scholes engine's drift shift, for any script and any model that
 * describes its increments. Written a_f = theta_f sqrt(T), the objective is
 * log G(path at z = mu(a)) - |a|^2 / 2: one path per evaluation (the drifted
 * mean path), maximised by coordinate search on a grid of a in [-5, 5] (step
 * 0.5) then golden section: ~70 path evaluations for one factor.
 *
 * GHS's mode is a heuristic, not a guarantee: the caller runs a pilot with
 * and without the drift and keeps it only when it lowers the variance
 * (importance_sampling_pays).
 */

namespace quantModeling::mc
{

    struct ImportanceDrift
    {
        std::vector<double> theta; ///< per Brownian factor; empty: no drift
        std::vector<double> a;     ///< theta_f sqrt(T): the shift in terminal standard deviations
        std::string why_none;      ///< when theta is empty
    };

    /**
     * @brief GHS drift for the first payoff of `product` on `model` (init()
     *        already called).
     */
    template <class T>
    ImportanceDrift find_importance_drift(const ISimulatableProduct<T> &product, const ISimulationModel<T> &model)
    {
        ImportanceDrift out;
        const std::size_t dim = model.sim_dim();
        const BrownianLayout layout = model.brownian_layout();
        if (!layout.covers(dim))
        {
            out.why_none = "the model does not describe its Brownian increments";
            return out;
        }
        const std::size_t F = layout.factors, n = layout.times.size();
        const double T_end = layout.times.back();

        Scenario<T> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<T> pay(product.payoff_labels().size());
        std::vector<double> gauss(dim, 0.0);
        auto objective = [&](const std::vector<double> &a)
        {
            double prev = 0.0, norm2 = 0.0;
            for (std::size_t d = 0; d < n; ++d)
            {
                const double dt = layout.times[d] - prev;
                prev = layout.times[d];
                for (std::size_t f = 0; f < F; ++f)
                    gauss[d * layout.stride + f] = a[f] / std::sqrt(T_end) * std::sqrt(dt);
            }
            for (double x : a)
                norm2 += x * x;
            model.generate_path(std::span<const double>(gauss.data(), dim), path);
            product.payoffs(path, pay);
            const double G = static_cast<double>(pay.front());
            if (!(G > 0.0) || !std::isfinite(G))
                return -std::numeric_limits<double>::infinity();
            return std::log(G) - 0.5 * norm2;
        };

        std::vector<double> a(F, 0.0);
        double best = objective(a);
        // One sweep suffices for one factor; coordinates interact otherwise.
        constexpr double kLo = -5.0, kHi = 5.0, kStep = 0.5;
        const int sweeps = F == 1 ? 1 : 3;
        for (int sweep = 0; sweep < sweeps; ++sweep)
            for (std::size_t f = 0; f < F; ++f)
            {
                std::vector<double> trial = a;
                double best_x = a[f];
                for (double x = kLo; x <= kHi + 1e-12; x += kStep)
                {
                    trial[f] = x;
                    const double v = objective(trial);
                    if (v > best)
                    {
                        best = v;
                        best_x = x;
                    }
                }
                // golden section around the grid's best
                double lo = best_x - kStep, hi = best_x + kStep;
                const double g = 0.5 * (std::sqrt(5.0) - 1.0);
                for (int it = 0; it < 24; ++it)
                {
                    const double m1 = hi - g * (hi - lo), m2 = lo + g * (hi - lo);
                    trial[f] = m1;
                    const double v1 = objective(trial);
                    trial[f] = m2;
                    const double v2 = objective(trial);
                    if (v1 >= v2)
                        hi = m2;
                    else
                        lo = m1;
                    if (std::max(v1, v2) > best)
                    {
                        best = std::max(v1, v2);
                        best_x = v1 >= v2 ? m1 : m2;
                    }
                }
                a[f] = best_x;
            }
        if (!std::isfinite(best))
        {
            out.why_none = "the payoff is zero along every drifted mean path";
            return out;
        }
        double norm = 0.0;
        for (double x : a)
            norm += x * x;
        if (std::sqrt(norm) < 0.05)
        {
            out.why_none = "the payoff's mode is at zero drift";
            return out;
        }
        out.a = a;
        for (double x : a)
            out.theta.push_back(x / std::sqrt(T_end));
        return out;
    }

    /// Keep the drift when a pilot's variance falls by at least 5 %.
    inline bool importance_sampling_pays(double pilot_plain_se, double pilot_is_se)
    {
        return pilot_is_se > 0.0 && pilot_is_se < 0.95 * pilot_plain_se;
    }

    /// "importance sampling (drift a = 2.31 sd, pilot variance / 14.2)".
    inline std::string importance_note(const ImportanceDrift &d, double pilot_plain_se, double pilot_is_se)
    {
        std::string a;
        char buf[48];
        for (std::size_t i = 0; i < d.a.size(); ++i)
        {
            std::snprintf(buf, sizeof buf, "%s%.2f", i ? ", " : "", d.a[i]);
            a += buf;
        }
        const double ratio = pilot_is_se > 0.0 ? (pilot_plain_se * pilot_plain_se) / (pilot_is_se * pilot_is_se) : 0.0;
        std::snprintf(buf, sizeof buf, "%.1f", ratio);
        return " + importance sampling (drift " + a + " sd, pilot variance / " + buf + ")";
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_IMPORTANCE_SAMPLING_HPP
