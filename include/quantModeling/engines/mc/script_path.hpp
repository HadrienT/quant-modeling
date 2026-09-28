#ifndef QM_ENGINES_MC_SCRIPT_PATH_HPP
#define QM_ENGINES_MC_SCRIPT_PATH_HPP

#include <cmath>
#include <cstdint>
#include <type_traits>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/path_draws.hpp"
#include "quantModeling/engines/mc/spot_controls.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/scripting/bytecode.hpp"

/**
 * @file script_path.hpp
 * @brief One path of a compiled script on a DeviceModel -- host and device
 *        (blueprint/wp/19-gpu.md §3-§6, lots G2-G3).
 *
 * The body of the GPU script kernel, as a free function: it steps the model
 * (the shared functors of path_steps.hpp, or Black-Scholes's coefficients),
 * and at each event date runs the event's bytecode through the CPU's own
 * interpreter. Written once, it runs in the kernel and on the host, so the
 * GPU engines' per-path code is exercised by the CPU test suite too.
 *
 * Templated on the number type: T = double prices (lot G2, unchanged); T =
 * Dual<N> carries N parameter derivatives forward through the model and the
 * script (lot G3, blueprint §6.1), for the models with few parameters --
 * Black-Scholes (one or several assets) and Heston. The local-vol and SLV
 * grids only run in double (their risks take the per-path adjoint,
 * engines/mc/script_adjoint.hpp, or stay on the CPU).
 */

namespace quantModeling::mc
{

    /// Fixed per-path sizes of the device machine (a script or model beyond
    /// them stays on the CPU).
    struct ScriptLimits
    {
        static constexpr int kVars = 32;
        static constexpr int kStack = 16;
        static constexpr int kDegrees = 8;
        static constexpr int kIfSlots = 64;
        static constexpr int kIfModes = 8;
        static constexpr int kAssets = 8;
        static constexpr int kDiscounts = 8;
    };

    /// A DeviceModel and a compiled script as flat pointers -- host or
    /// device memory, whichever side runs the path.
    struct ScriptPathView
    {
        scripting::ProgramView prog;
        const std::int32_t *event_begin = nullptr;
        int n_vars = 0;
        const Real *baseline = nullptr; ///< null: zeros

        DeviceModel::Kind kind = DeviceModel::Kind::BlackScholes;
        int n_assets = 1;
        Real r = 0.0, q = 0.0;
        const Real *s0 = nullptr;
        const Real *chol = nullptr;
        const Real *divs = nullptr; ///< per asset (Black-Scholes)
        const Real *vols = nullptr; ///< per asset (Black-Scholes)
        HestonParamsT<Real> heston{0.0, 0.0, 0.0, 0.0, 0.0};
        GridView<Real> grid;
        const Time *t = nullptr;
        const int *draws = nullptr;
        const int *event = nullptr;
        const Real *drift = nullptr;
        const Real *vol_sqrt_dt = nullptr;
        int n_steps = 0;
        int factors = 1;
        int stride = 1;

        const int *disc_begin = nullptr; ///< event e: disc_mats[disc_begin[e] .. disc_begin[e + 1])
        const Time *disc_mats = nullptr;

        SpotControls controls;
    };

    /// The model inputs a path is a function of, as T. For T = Dual<N> the
    /// caller seeds each parameter's direction (script_dual_inputs).
    template <class T>
    struct ScriptModelInputs
    {
        T s0[ScriptLimits::kAssets];
        T divs[ScriptLimits::kAssets];
        T vols[ScriptLimits::kAssets];
        T r, q;
        HestonParamsT<T> heston;
    };

    template <class T>
    QM_HOST_DEVICE ScriptModelInputs<T> script_inputs(const ScriptPathView &v)
    {
        ScriptModelInputs<T> in;
        for (int k = 0; k < v.n_assets && k < ScriptLimits::kAssets; ++k)
        {
            in.s0[k] = T(v.s0[k]);
            in.divs[k] = T(v.divs ? v.divs[k] : 0.0);
            in.vols[k] = T(v.vols ? v.vols[k] : 0.0);
        }
        in.r = T(v.r);
        in.q = T(v.q);
        in.heston = {T(v.heston.v0), T(v.heston.kappa), T(v.heston.theta), T(v.heston.xi), T(v.heston.rho)};
        return in;
    }

    /**
     * @brief Simulate one path and run the script on it; return its
     *        (numeraire-deflated) payoff.
     *
     * `draws` must have been begun on the path's unit; `sign` = -1 is the
     * antithetic mirror. `x`, when not null, receives the spot controls.
     */
    template <class T>
    QM_HOST_DEVICE T script_path(const ScriptPathView &v, const ScriptModelInputs<T> &in, PathDraws &draws,
                                 double sign, Real *x)
    {
        using L = ScriptLimits;
        using std::exp;
        using std::max;
        using std::sqrt;
        constexpr bool kPlain = std::is_same_v<T, double>;

        T vars[L::kVars], stack[L::kStack], degrees[L::kDegrees], slots[L::kIfSlots];
        int modes[L::kIfModes];
        T S[L::kAssets], spots[L::kAssets], disc[L::kDiscounts];

        for (int i = 0; i < v.n_vars; ++i)
            vars[i] = v.baseline ? T(v.baseline[i]) : T(0.0);
        scripting::Machine<T> m{vars, stack, degrees, slots, modes, T(0.0)};
        for (int k = 0; k < v.n_assets; ++k)
            S[k] = in.s0[k];
        T var = in.heston.v0;

        Time t_prev = 0.0;
        int d = 0; // drawing steps so far
        for (int s = 0; s < v.n_steps; ++s)
        {
            const Time ts = v.t[s];
            const Time dt = ts - t_prev;
            if (v.draws[s])
            {
                switch (v.kind)
                {
                    case DeviceModel::Kind::BlackScholes:
                        if (v.n_assets == 1)
                        {
                            const double z = sign * draws(d, 0);
                            if constexpr (kPlain)
                                S[0] *= exp(v.drift[s] + v.vol_sqrt_dt[s] * z);
                            else
                            {
                                // BlackScholesSimModel::init: mu dt, sigma sqrt(dt)
                                const T mu = in.r - in.divs[0] - 0.5 * in.vols[0] * in.vols[0];
                                S[0] *= exp(mu * dt + in.vols[0] * sqrt(dt) * z);
                            }
                        }
                        else
                        {
                            // MultiAssetBSSimModel::generate_path: z = chol * u, full rows
                            double w[L::kAssets];
                            for (int k = 0; k < v.n_assets; ++k)
                                w[k] = sign * draws(d, k);
                            for (int a = 0; a < v.n_assets; ++a)
                            {
                                double acc = 0.0;
                                for (int c = 0; c < v.n_assets; ++c)
                                    acc += v.chol[a * v.n_assets + c] * w[c];
                                if constexpr (kPlain)
                                    S[a] *= exp(v.drift[s * v.n_assets + a] + v.vol_sqrt_dt[s * v.n_assets + a] * acc);
                                else
                                {
                                    const T mu = in.r - in.divs[a] - 0.5 * in.vols[a] * in.vols[a];
                                    S[a] *= exp(mu * dt + in.vols[a] * sqrt(dt) * acc);
                                }
                            }
                        }
                        break;
                    case DeviceModel::Kind::LocalVol:
                        if constexpr (kPlain)
                            local_vol_step(v.grid, v.r, v.q, S[0], ts, dt, sign * draws(d, 0));
                        break;
                    case DeviceModel::Kind::SLV:
                        if constexpr (kPlain)
                        {
                            const double z0 = sign * draws(d, 0);
                            const double z1 = sign * draws(d, 1);
                            slv_step(v.grid, v.heston, v.r, v.q, S[0], var, ts, dt, z0, z1);
                        }
                        break;
                    case DeviceModel::Kind::Heston:
                    {
                        const double z0 = sign * draws(d, 0);
                        const double z1 = sign * draws(d, 1);
                        const double sqdt = sqrt(dt);
                        const T v_plus = max(var, variance_floor());
                        const T sqrt_v_plus = sqrt(v_plus);
                        const T zero(0.0);
                        S[0] = S[0] * exp(heston_log_return(in.r, in.q, zero, zero, v_plus, sqrt_v_plus, sqdt, dt, z0));
                        heston_variance_step(in.heston, var, v_plus, sqrt_v_plus, sqdt, dt, z0, z1);
                        break;
                    }
                }
                ++d;
            }
            t_prev = ts;

            const int e = v.event[s];
            if (e < 0)
                continue;
            for (int k = 0; k < v.n_assets; ++k)
                spots[k] = S[k];
            int nd = 0;
            for (int i = v.disc_begin[e]; i < v.disc_begin[e + 1]; ++i)
                disc[nd++] = exp(-in.r * (v.disc_mats[i] - ts));
            const T numeraire = exp(in.r * ts);
            scripting::run_event(v.prog, v.event_begin[e], v.event_begin[e + 1], m, spots, disc, numeraire);
            if (x)
                for (int c = 0; c < v.controls.n; ++c)
                    if (v.controls.event[c] == e)
                        x[c] = value_of(S[v.controls.asset[c]]) / value_of(numeraire);
        }
        return m.payoff;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SCRIPT_PATH_HPP
