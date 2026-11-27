#ifndef QM_ENGINES_MC_SCRIPT_PATH_HOST_HPP
#define QM_ENGINES_MC_SCRIPT_PATH_HOST_HPP

#include <cmath>
#include <cstddef>
#include <vector>

#include "quantModeling/engines/mc/script_path.hpp"

/**
 * @file script_path_host.hpp
 * @brief A ScriptPathView over host memory: the GPU engines' per-path code
 *        run on the CPU, which is how the CPU test suite exercises it
 *        (blueprint/wp/19-gpu.md §10) -- the kernel builds the same view over
 *        device copies (src/gpu/script.cu).
 *
 * The program, model and baseline must outlive the view.
 */

namespace quantModeling::mc
{

    /**
     * The event tables of a model on a forward curve (WP 32 F4), on the
     * host: per event, its numeraire e^{r t} / DF(t) and, per df() slot
     * disc_mats[disc_begin[e] .. disc_begin[e + 1]), DF(T) / DF(t) e^{-r (T -
     * t)} -- the CPU model's expressions (ModelCurve::write_rates), so the
     * device's paths read the CPU's bits. t is the step's time of the event.
     */
    inline void curve_event_tables(const DeviceModel &dm, const std::vector<int> &disc_begin,
                                   const std::vector<Time> &disc_mats, std::size_t n_events,
                                   std::vector<Real> &numeraire, std::vector<Real> &disc)
    {
        const ForwardCurve &c = *dm.curve;
        numeraire.assign(n_events, 0.0);
        disc.assign(disc_mats.size(), 0.0);
        for (std::size_t s = 0; s < dm.t.size(); ++s)
        {
            const int e = dm.event[s];
            if (e < 0 || static_cast<std::size_t>(e) >= n_events)
                continue;
            const Time ts = dm.t[s];
            const Real df_t = c.discount(ts);
            for (int i = disc_begin[static_cast<std::size_t>(e)]; i < disc_begin[static_cast<std::size_t>(e) + 1]; ++i)
            {
                const Time m = disc_mats[static_cast<std::size_t>(i)];
                disc[static_cast<std::size_t>(i)] = (c.discount(m) / df_t) * std::exp(-dm.r * (m - ts));
            }
            numeraire[static_cast<std::size_t>(e)] = (1.0 / df_t) * std::exp(dm.r * ts);
        }
    }

    struct ScriptPathHost
    {
        std::vector<int> disc_begin{0};
        std::vector<Time> disc_mats;
        std::vector<Real> curve_numeraire, curve_disc; ///< a model on a forward curve's event tables
        ScriptPathView view;

        ScriptPathHost(const scripting::Program &p, const DeviceModel &dm,
                       const std::vector<std::vector<Time>> &discount_mats, const std::vector<Real> &baseline)
        {
            for (const auto &mats : discount_mats)
            {
                disc_mats.insert(disc_mats.end(), mats.begin(), mats.end());
                disc_begin.push_back(static_cast<int>(disc_mats.size()));
            }
            view.prog = p.view();
            view.event_begin = p.event_begin.data();
            view.n_vars = p.n_vars;
            view.baseline = baseline.empty() ? nullptr : baseline.data();
            view.kind = dm.kind;
            view.n_assets = dm.n_assets;
            view.r = dm.r;
            view.q = dm.q;
            view.s0 = dm.s0.data();
            view.chol = dm.chol.empty() ? nullptr : dm.chol.data();
            view.divs = dm.divs.empty() ? nullptr : dm.divs.data();
            view.vols = dm.vols.empty() ? nullptr : dm.vols.data();
            view.heston = dm.heston;
            view.grid = {dm.K.empty() ? nullptr : dm.K.data(), static_cast<int>(dm.K.size()),
                         dm.T_grid.empty() ? nullptr : dm.T_grid.data(), static_cast<int>(dm.T_grid.size()),
                         dm.grid.empty() ? nullptr : dm.grid.data()};
            view.t = dm.t.data();
            view.draws = dm.draws.data();
            view.event = dm.event.data();
            view.drift = dm.drift.empty() ? nullptr : dm.drift.data();
            view.vol_sqrt_dt = dm.vol_sqrt_dt.empty() ? nullptr : dm.vol_sqrt_dt.data();
            view.n_steps = static_cast<int>(dm.t.size());
            view.factors = dm.factors;
            view.stride = dm.stride;
            view.variance_scheme = dm.variance_scheme;
            view.disc_begin = disc_begin.data();
            view.disc_mats = disc_mats.empty() ? nullptr : disc_mats.data();
            if (dm.curve) // WP 32 F4: the curve's carry per step and its event tables
            {
                curve_event_tables(dm, disc_begin, disc_mats, p.n_events(), curve_numeraire, curve_disc);
                view.step_carry = dm.step_carry.empty() ? nullptr : dm.step_carry.data();
                view.event_numeraire = curve_numeraire.data();
                view.event_disc = curve_disc.empty() ? nullptr : curve_disc.data();
            }
        }

        ScriptPathHost(const ScriptPathHost &) = delete; // view points into *this
        ScriptPathHost &operator=(const ScriptPathHost &) = delete;
    };

    /// The drawing steps' end times (for stratified draws): step s of the
    /// device grid where draws[s] is set.
    inline std::vector<Time> drawing_times(const DeviceModel &dm)
    {
        std::vector<Time> out;
        for (std::size_t s = 0; s < dm.t.size(); ++s)
            if (dm.draws[s])
                out.push_back(dm.t[s]);
        return out;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SCRIPT_PATH_HOST_HPP
