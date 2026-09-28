#ifndef QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP
#define QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP

#include <cstdint>

#include "quantModeling/engines/mc/sobol_bridge.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"

namespace quantModeling::mc
{

    /**
     * @brief The flat tables of a Sobol RQMC run of `replicates` replicates:
     *        the generic engine's scheme -- replicate b is
     *        SobolSequence(dim, (seed << 32) | b) -- and, when `bridge` is
     *        set and the model describes its increments, its Brownian bridge
     *        (BridgedGaussians' own tables).
     */
    inline SobolTables sobol_tables(std::size_t dimension, uint64_t seed, int replicates,
                                    const BrownianLayout &layout, bool bridge)
    {
        SobolTables t;
        t.dim = static_cast<int>(dimension);
        t.replicates = replicates;
        for (int b = 0; b < replicates; ++b)
        {
            const uint64_t batch_seed =
                (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) | static_cast<uint64_t>(b);
            const SobolSequence s(t.dim, batch_seed);
            if (b == 0)
                t.V.assign(s.directions().begin(), s.directions().end());
            t.shift.insert(t.shift.end(), s.shifts().begin(), s.shifts().end());
        }
        if (bridge && layout.covers(dimension))
        {
            const BridgedGaussians g(layout.times, layout.factors, layout.stride);
            const BrownianBridge &br = g.bridge();
            t.bridge_index = br.bridge_index();
            t.left_index = br.left_index();
            t.right_index = br.right_index();
            t.left_weight = br.left_weight();
            t.right_weight = br.right_weight();
            t.std_dev = br.std_dev();
            t.inv_sqrt_dt = g.inv_sqrt_dt();
            t.n = static_cast<int>(layout.times.size());
            t.factors = static_cast<int>(layout.factors);
            t.stride = static_cast<int>(layout.stride);
        }
        return t;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP
