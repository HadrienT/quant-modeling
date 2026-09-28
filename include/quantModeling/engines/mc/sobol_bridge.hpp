#ifndef QM_ENGINES_MC_SOBOL_BRIDGE_HPP
#define QM_ENGINES_MC_SOBOL_BRIDGE_HPP

#include <cstdint>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/sobol.hpp"

/**
 * @file sobol_bridge.hpp
 * @brief A Sobol point, through the Brownian bridge, as a model's gaussians --
 *        host and device (blueprint/wp/19-gpu.md §2.2, §2.4).
 *
 * What the generic engine does with SobolSequence and BridgedGaussians, point
 * by point and in place, on flat tables: point p of replicate b is
 * gray(p)'s XOR of the Joe-Kuo directions, XORed with replicate b's digital
 * shift (the shifts SobolSequence(dim, seed_b) draws), mapped by Phi^-1;
 * the bridge then consumes its coordinates by order of importance -- every
 * factor's W(T) first -- and hands back the model's increments in time
 * order. Same integers as the CPU, same arithmetic: the GPU's gaussians are
 * the CPU's up to the last ulps of the bridge's multiply-adds.
 *
 * The bisection bridge needs the whole point before it writes the first
 * increment: `dim` doubles per path. On the device they live in per-thread
 * scratch in global memory (element j at base[j * stride], coalesced across
 * a warp) -- a local array would make the driver reserve it for every
 * thread the card can hold.
 */

namespace quantModeling::mc
{

    struct SobolBridgeView
    {
        const uint32_t *V = nullptr;     ///< dim x 32 direction integers, dimension-major
        const uint32_t *shift = nullptr; ///< replicates x dim digital shifts
        int dim = 0;

        // Bridge (n == 0: the point is already in the model's time order).
        int n = 0, factors = 0, stride = 0;
        const int *bridge_index = nullptr, *left_index = nullptr, *right_index = nullptr;
        const double *left_weight = nullptr, *right_weight = nullptr, *std_dev = nullptr, *inv_sqrt_dt = nullptr;
    };

    /**
     * @brief Point p of replicate `rep` as the model's gaussians, time order,
     *        into out[j * os]; pt[j * ps] is scratch for the raw point.
     */
    QM_HOST_DEVICE inline void sobol_bridged_gaussians(const SobolBridgeView &v, uint64_t rep, uint32_t p,
                                                       double *pt, long ps, double *out, long os)
    {
        const uint32_t *sh = v.shift + rep * static_cast<uint64_t>(v.dim);
        auto coord = [&](int j)
        { return inverse_normal_cdf(sobol_to_uniform(sobol_point_bits(v.V + 32L * j, p), sh[j])); };
        if (v.n == 0)
        {
            for (int j = 0; j < v.dim; ++j)
                out[j * os] = coord(j);
            return;
        }
        for (int j = 0; j < v.dim; ++j)
            pt[j * ps] = coord(j);

        // BridgedGaussians::map, in place: W(t_s) of factor f is written into
        // the slot of its increment, then turned into increments backwards.
        const int extra = v.stride - v.factors;
        for (int f = 0; f < v.factors; ++f)
        {
            auto w = [&](int s) -> double &
            { return out[(static_cast<long>(s) * v.stride + f) * os]; };
            auto z = [&](int i)
            { return pt[(static_cast<long>(i) * v.factors + f) * ps]; };
            w(v.n - 1) = v.std_dev[0] * z(0);
            for (int i = 1; i < v.n; ++i)
            {
                const int j = v.left_index[i];
                const int k = v.right_index[i];
                const double wj = (j != 0) ? w(j - 1) : 0.0;
                w(v.bridge_index[i]) = v.left_weight[i] * wj + v.right_weight[i] * w(k) + v.std_dev[i] * z(i);
            }
            for (int s = v.n - 1; s >= 0; --s)
            {
                const double prev = s > 0 ? w(s - 1) : 0.0;
                w(s) = (w(s) - prev) * v.inv_sqrt_dt[s];
            }
        }
        for (int s = 0; s < v.n; ++s)
            for (int e = 0; e < extra; ++e)
                out[(static_cast<long>(s) * v.stride + v.factors + e) * os] =
                    pt[(static_cast<long>(v.n) * v.factors + static_cast<long>(s) * extra + e) * ps];
    }

    /// The tables of a Sobol RQMC run, in host memory (built by
    /// sobol_tables(), engines/mc/sobol_bridge_host.hpp).
    struct SobolTables
    {
        std::vector<uint32_t> V, shift;
        std::vector<int> bridge_index, left_index, right_index;
        std::vector<double> left_weight, right_weight, std_dev, inv_sqrt_dt;
        int dim = 0, n = 0, factors = 0, stride = 0;
        int replicates = 0;

        bool bridged() const { return n > 0; }

        /// A view over this host memory.
        SobolBridgeView view() const
        {
            SobolBridgeView v;
            v.V = V.data();
            v.shift = shift.data();
            v.dim = dim;
            v.n = n;
            v.factors = factors;
            v.stride = stride;
            if (n > 0)
            {
                v.bridge_index = bridge_index.data();
                v.left_index = left_index.data();
                v.right_index = right_index.data();
                v.left_weight = left_weight.data();
                v.right_weight = right_weight.data();
                v.std_dev = std_dev.data();
                v.inv_sqrt_dt = inv_sqrt_dt.data();
            }
            return v;
        }
    };

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SOBOL_BRIDGE_HPP
