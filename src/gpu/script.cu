#include "quantModeling/gpu/script.hpp"
#include "quantModeling/utils/philox.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        using L = ScriptLimits;

        struct ScriptUnit
        {
            scripting::ProgramView prog;
            const std::int32_t *event_begin;
            int n_vars;
            const Real *baseline; ///< null: zeros

            DeviceModel::Kind kind;
            int n_assets;
            Real r, q;
            const Real *s0;
            const Real *chol;
            mc::HestonParamsT<Real> heston;
            mc::GridView<Real> grid;
            const Time *t;
            const int *draws;
            const int *event;
            const Real *drift;
            const Real *vol_sqrt_dt;
            int n_steps;
            int factors;
            int stride;

            const int *disc_begin; ///< event e: disc_mats[disc_begin[e] .. disc_begin[e + 1])
            const Time *disc_mats;

            uint64_t seed;
            bool antithetic;

            __device__ double z(uint64_t u, uint32_t j, double sign) const
            {
                return sign * inverse_normal_cdf(philox_uniform(seed, u, j));
            }

            /// One path; sign = −1 is its antithetic mirror.
            __device__ double path(uint64_t u, double sign) const
            {
                Real vars[L::kVars], stack[L::kStack], degrees[L::kDegrees], slots[L::kIfSlots];
                int modes[L::kIfModes];
                Real S[L::kAssets], spots[L::kAssets], disc[L::kDiscounts];

                for (int i = 0; i < n_vars; ++i)
                    vars[i] = baseline ? baseline[i] : Real(0);
                scripting::Machine<Real> m{vars, stack, degrees, slots, modes, Real(0)};
                for (int k = 0; k < n_assets; ++k)
                    S[k] = s0[k];
                Real v = heston.v0;

                Time t_prev = 0.0;
                uint32_t d = 0; // drawing steps so far
                for (int s = 0; s < n_steps; ++s)
                {
                    const Time ts = t[s];
                    const Time dt = ts - t_prev;
                    if (draws[s])
                    {
                        const uint32_t j = d * static_cast<uint32_t>(stride);
                        switch (kind)
                        {
                            case DeviceModel::Kind::BlackScholes:
                                if (n_assets == 1)
                                {
                                    S[0] *= exp(drift[s] + vol_sqrt_dt[s] * z(u, j, sign));
                                }
                                else
                                {
                                    // MultiAssetBSSimModel::generate_path: z = chol * u, full rows
                                    double w[L::kAssets];
                                    for (int k = 0; k < n_assets; ++k)
                                        w[k] = z(u, j + static_cast<uint32_t>(k), sign);
                                    for (int a = 0; a < n_assets; ++a)
                                    {
                                        double acc = 0.0;
                                        for (int c = 0; c < n_assets; ++c)
                                            acc += chol[a * n_assets + c] * w[c];
                                        S[a] *= exp(drift[s * n_assets + a] + vol_sqrt_dt[s * n_assets + a] * acc);
                                    }
                                }
                                break;
                            case DeviceModel::Kind::LocalVol:
                                mc::local_vol_step(grid, r, q, S[0], ts, dt, z(u, j, sign));
                                break;
                            case DeviceModel::Kind::SLV:
                                mc::slv_step(grid, heston, r, q, S[0], v, ts, dt, z(u, j, sign), z(u, j + 1, sign));
                                break;
                            case DeviceModel::Kind::Heston:
                            {
                                const double z0 = z(u, j, sign), z1 = z(u, j + 1, sign);
                                const double sqdt = sqrt(dt);
                                const Real v_plus = fmax(v, mc::variance_floor());
                                const Real sqrt_v_plus = sqrt(v_plus);
                                const Real zero = 0.0;
                                S[0] = S[0] *
                                       exp(mc::heston_log_return(r, q, zero, zero, v_plus, sqrt_v_plus, sqdt, dt, z0));
                                mc::heston_variance_step(heston, v, v_plus, sqrt_v_plus, sqdt, dt, z0, z1);
                                break;
                            }
                        }
                        ++d;
                    }
                    t_prev = ts;

                    const int e = event[s];
                    if (e < 0)
                        continue;
                    for (int k = 0; k < n_assets; ++k)
                        spots[k] = S[k];
                    int nd = 0;
                    for (int i = disc_begin[e]; i < disc_begin[e + 1]; ++i)
                        disc[nd++] = exp(-r * (disc_mats[i] - ts));
                    const Real numeraire = exp(r * ts);
                    scripting::run_event(prog, event_begin[e], event_begin[e + 1], m, spots, disc, numeraire);
                }
                return m.payoff;
            }

            __device__ double operator()(uint64_t u) const
            {
                const double a = path(u, 1.0);
                if (!antithetic)
                    return a;
                const double b = path(u, -1.0);
                return 0.5 * (a + b);
            }
        };

        /// Device copies of host arrays, freed together.
        struct DeviceArrays
        {
            std::vector<void *> owned;
            ~DeviceArrays()
            {
                for (void *p : owned)
                    cudaFree(p);
            }
            template <class T>
            const T *put(const std::vector<T> &h)
            {
                if (h.empty())
                    return nullptr;
                T *d = nullptr;
                detail::check(cudaMalloc(&d, h.size() * sizeof(T)), "cudaMalloc");
                owned.push_back(d);
                detail::check(cudaMemcpy(d, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice), "cudaMemcpy");
                return d;
            }
        };
    } // namespace

    WelfordAccumulator simulate_script(const ScriptGpuRequest &req)
    {
        if (!req.program || !req.model)
            throw InvalidInput("simulate_script: program and model are required");
        const scripting::Program &p = *req.program;
        const DeviceModel &dm = *req.model;
        const std::string why = script_gpu_unsupported(p, dm, req.discount_mats);
        if (!why.empty())
            throw InvalidInput("GPU: " + why);
        if (req.discount_mats.size() != p.n_events())
            throw InvalidInput("simulate_script: one discount list per event");
        detail::require_device(req.device);
        detail::check(cudaSetDevice(req.device), "cudaSetDevice");

        std::vector<int> disc_begin{0};
        std::vector<Time> disc_mats;
        for (const auto &mats : req.discount_mats)
        {
            disc_mats.insert(disc_mats.end(), mats.begin(), mats.end());
            disc_begin.push_back(static_cast<int>(disc_mats.size()));
        }

        DeviceArrays mem;
        ScriptUnit unit{};
        unit.prog = {mem.put(p.code), mem.put(p.ifs), mem.put(p.aff)};
        unit.event_begin = mem.put(p.event_begin);
        unit.n_vars = p.n_vars;
        unit.baseline = mem.put(req.baseline);
        unit.kind = dm.kind;
        unit.n_assets = dm.n_assets;
        unit.r = dm.r;
        unit.q = dm.q;
        unit.s0 = mem.put(dm.s0);
        unit.chol = mem.put(dm.chol);
        unit.heston = dm.heston;
        unit.grid = {mem.put(dm.K), static_cast<int>(dm.K.size()), mem.put(dm.T_grid),
                     static_cast<int>(dm.T_grid.size()), mem.put(dm.grid)};
        unit.t = mem.put(dm.t);
        unit.draws = mem.put(dm.draws);
        unit.event = mem.put(dm.event);
        unit.drift = mem.put(dm.drift);
        unit.vol_sqrt_dt = mem.put(dm.vol_sqrt_dt);
        unit.n_steps = static_cast<int>(dm.t.size());
        unit.factors = dm.factors;
        unit.stride = dm.stride;
        unit.disc_begin = mem.put(disc_begin);
        unit.disc_mats = mem.put(disc_mats);
        unit.seed = req.seed;
        unit.antithetic = req.antithetic;

        return detail::run_logical_blocks<WelfordAccumulator>(req.device, req.n_units, unit, req.max_blocks_per_launch);
    }

} // namespace quantModeling::gpu
