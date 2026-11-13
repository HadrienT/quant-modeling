#ifndef QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP
#define QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "quantModeling/engines/mc/sobol_bridge.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"

namespace quantModeling::mc
{

    /**
     * @brief The flat tables of a Sobol RQMC run whose replicate b is
     *        SobolSequence(dim, keys[b]) and, when `bridge` is set and the
     *        model describes its increments, its Brownian bridge
     *        (BridgedGaussians' own tables).
     */
    inline SobolTables sobol_tables(std::size_t dimension, const std::vector<uint64_t> &keys,
                                    const BrownianLayout &layout, bool bridge, bool owen = false)
    {
        SobolTables t;
        t.dim = static_cast<int>(dimension);
        t.owen = owen;
        t.replicates = static_cast<int>(keys.size());
        // The directions are the same for every replicate: built once.
        {
            const SobolSequence s(t.dim);
            t.V.assign(s.directions().begin(), s.directions().end());
        }
        for (const uint64_t key : keys)
        {
            const std::vector<uint32_t> shift = SobolSequence::shifts_for(t.dim, key);
            t.shift.insert(t.shift.end(), shift.begin(), shift.end());
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

    /// The generic engine's scheme: replicate b is
    /// SobolSequence(dim, (seed << 32) | b).
    inline SobolTables sobol_tables(std::size_t dimension, uint64_t seed, int replicates,
                                    const BrownianLayout &layout, bool bridge, bool owen = false)
    {
        std::vector<uint64_t> keys;
        for (int b = 0; b < replicates; ++b)
            keys.push_back((static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) | static_cast<uint64_t>(b));
        return sobol_tables(dimension, keys, layout, bridge, owen);
    }

    /// The Sobol tables of one multilevel QMC level: its fine grid, and the
    /// coarse grid driven by the same points.
    struct CoupledSobolTables
    {
        SobolTables fine, coarse;
    };

    /**
     * @brief The tables that couple a multilevel level's fine and coarse
     *        paths through the point itself (WP 26 P6, §10).
     *
     * `fine` halves every step of `coarse`. The fine grid's bridge is built
     * in a nested order: its first n_c variates construct W on the coarse
     * dates exactly as the coarse grid's own bridge does (same indices, same
     * weights), the n_c others then fill each coarse step's midpoint. The
     * coarse tables are the coarse bridge on the first n_c * factors
     * coordinates of the same randomised sequences. Point p of replicate b
     * therefore gives both grids the same Brownian path: the coarse
     * increments are the sums of the fine ones, as LevelSampler builds
     * them, without the two paths sharing a kernel -- the fine and the
     * coarse sums are two ordinary runs, and a replicate's sample of the
     * correction is the difference of its two means.
     *
     * Replicate b's sequence is SobolSequence(fine dim, keys[b]). Throws
     * InvalidInput when the layouts do not nest this way or carry draws
     * other than Brownian increments (a jump's: their place in the point
     * depends on the grid).
     */
    inline CoupledSobolTables coupled_sobol_tables(const BrownianLayout &fine, const BrownianLayout &coarse,
                                                   const std::vector<uint64_t> &keys, bool owen = false)
    {
        const std::size_t nc = coarse.times.size(), nf = fine.times.size(), factors = fine.factors;
        if (fine.empty() || coarse.empty() || factors != coarse.factors || fine.stride != factors ||
            coarse.stride != factors || nf != 2 * nc)
            throw InvalidInput("multilevel QMC: the levels' draws do not nest (Brownian increments only, every "
                               "coarse step halved)");
        for (std::size_t s = 0; s < nc; ++s)
            if (std::fabs(fine.times[2 * s + 1] - coarse.times[s]) > 1e-12 * std::max(1.0, coarse.times[s]))
                throw InvalidInput("multilevel QMC: a coarse date is not a fine date");

        CoupledSobolTables t;
        SobolTables &f = t.fine, &c = t.coarse;
        f.dim = static_cast<int>(nf * factors);
        c.dim = static_cast<int>(nc * factors);
        f.owen = c.owen = owen;
        f.replicates = c.replicates = static_cast<int>(keys.size());
        {
            const SobolSequence seq(f.dim);
            f.V.assign(seq.directions().begin(), seq.directions().end());
            c.V.assign(f.V.begin(), f.V.begin() + 32L * c.dim);
        }
        for (const uint64_t key : keys)
        {
            const std::vector<uint32_t> shift = SobolSequence::shifts_for(f.dim, key);
            f.shift.insert(f.shift.end(), shift.begin(), shift.end());
            c.shift.insert(c.shift.end(), shift.begin(), shift.begin() + c.dim);
        }

        const BridgedGaussians gc(coarse.times, factors, factors);
        const BrownianBridge &bc = gc.bridge();
        c.bridge_index = bc.bridge_index();
        c.left_index = bc.left_index();
        c.right_index = bc.right_index();
        c.left_weight = bc.left_weight();
        c.right_weight = bc.right_weight();
        c.std_dev = bc.std_dev();
        c.inv_sqrt_dt = gc.inv_sqrt_dt();
        c.n = static_cast<int>(nc);
        f.n = static_cast<int>(nf);
        c.factors = c.stride = f.factors = f.stride = static_cast<int>(factors);

        // Coarse date s is fine date 2 s + 1; "left" j stands for the date
        // before j (0: time 0), so coarse j is fine 2 j.
        for (std::size_t i = 0; i < nc; ++i)
        {
            f.bridge_index.push_back(2 * c.bridge_index[i] + 1);
            f.left_index.push_back(2 * c.left_index[i]);
            f.right_index.push_back(i == 0 ? 0 : 2 * c.right_index[i] + 1);
            f.left_weight.push_back(c.left_weight[i]);
            f.right_weight.push_back(c.right_weight[i]);
            f.std_dev.push_back(c.std_dev[i]);
        }
        // Then the midpoint of each coarse step, between its two ends.
        for (std::size_t s = 0; s < nc; ++s)
        {
            const Time tj = s > 0 ? fine.times[2 * s - 1] : 0.0, tl = fine.times[2 * s], tk = fine.times[2 * s + 1];
            f.bridge_index.push_back(static_cast<int>(2 * s));
            f.left_index.push_back(static_cast<int>(2 * s));
            f.right_index.push_back(static_cast<int>(2 * s + 1));
            f.left_weight.push_back((tk - tl) / (tk - tj));
            f.right_weight.push_back((tl - tj) / (tk - tj));
            f.std_dev.push_back(std::sqrt((tl - tj) * (tk - tl) / (tk - tj)));
        }
        Time prev = 0.0;
        for (std::size_t s = 0; s < nf; ++s)
        {
            f.inv_sqrt_dt.push_back(1.0 / std::sqrt(fine.times[s] - prev));
            prev = fine.times[s];
        }
        return t;
    }
} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SOBOL_BRIDGE_HOST_HPP
