#ifndef QM_ENGINES_MC_SCRIPT_PATH_HOST_HPP
#define QM_ENGINES_MC_SCRIPT_PATH_HOST_HPP

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

    struct ScriptPathHost
    {
        std::vector<int> disc_begin{0};
        std::vector<Time> disc_mats;
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
            view.disc_begin = disc_begin.data();
            view.disc_mats = disc_mats.empty() ? nullptr : disc_mats.data();
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
