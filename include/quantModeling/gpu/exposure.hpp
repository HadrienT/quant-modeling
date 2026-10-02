#ifndef GPU_EXPOSURE_HPP
#define GPU_EXPOSURE_HPP

#include <cstdint>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/xva/exposure_program.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral_path.hpp"
#include "quantModeling/utils/accumulators.hpp"

/**
 * @file exposure.hpp
 * @brief The exposure engine on the device (blueprint/wp/23-xva.md §14.11,
 *        lot X7): one thread per path simulates the Hull-White state on the
 *        exposure grid and values every trade of an ExposureProgram at every
 *        date.
 *
 * Two entry points, for two uses:
 *
 *  - simulate_exposure_cube() brings the whole cube back to the host: every
 *    post-processing of risk/ (collateral, initial margin, wrong-way risk,
 *    capital, the report) then runs unchanged on it;
 *  - simulate_netting_set() nets the trades, applies the CSA and reduces the
 *    exposure **on the device**: only the profiles come back. The cube never
 *    exists, so the number of paths is no longer bounded by the host's
 *    memory, nor the time by the copy.
 *
 * Path p draws Φ⁻¹(Philox(seed, p, i)) at date i, as the CPU engine does: the
 * same scenarios, and the same values up to the last bits of exp and erfc
 * (the device's are not the host's). The paths are cut into logical blocks
 * (engines/mc/logical_blocks.hpp) shared between the devices, and every
 * reduction follows the block tree: the result is the same, bit for bit, on
 * one card or two and whatever the size of a launch.
 */

namespace quantModeling::gpu
{

    struct ExposureGpuRequest
    {
        /// Both must outlive the call.
        const xva::ExposureProgram *program = nullptr;
        const xva::StateDynamics *dynamics = nullptr;
        uint64_t seed = 0;
        uint64_t paths = 0;
        /// The devices that share the logical blocks; empty runs on device 0.
        std::vector<int> devices;
        /// Logical blocks (4 096 paths) per launch; 0 takes what half of the
        /// free memory holds. Tests set it to force several launches.
        uint64_t max_blocks_per_launch = 0;
    };

    /// Where the cube goes on the host: matrices of paths × dates, row by
    /// row, already allocated.
    struct ExposureCubeTarget
    {
        Real *discount_weight = nullptr;
        /// One per trade of the program: quantity × value.
        std::vector<Real *> values;
        /// One per trade, or empty when the cash flows are not wanted.
        std::vector<Real *> cashflows;
    };

    /// Fills the cube. @return how many devices ran.
    /// @throws GpuUnavailable without a device or on a CUDA error.
    int simulate_exposure_cube(const ExposureGpuRequest &request, const ExposureCubeTarget &target);

    /// One path's discounted netting-set value at one date, split in its
    /// positive and negative parts.
    struct ExposureSample
    {
        Real positive;
        Real negative;
        Real value;
    };

    /// Mean and variance over the paths of the three parts.
    struct ExposureDateStats
    {
        WelfordAccumulator positive, negative, value;

        QM_HOST_DEVICE void add(const ExposureSample &s)
        {
            positive.add(s.positive);
            negative.add(s.negative);
            value.add(s.value);
        }

        QM_HOST_DEVICE void merge(const ExposureDateStats &other)
        {
            positive.merge(other.positive);
            negative.merge(other.negative);
            value.merge(other.value);
        }
    };

    /// A netting set on the device: which trades, under which collateral.
    struct NettingSetGpuRequest
    {
        /// Indices into the program's trades, netted in this order.
        std::vector<int> trades;
        /// False: no collateral, and only plan.reporting is read (every date
        /// of the grid, for the engine).
        bool collateralised = false;
        CollateralPlan plan;
        /// Initial margin per reporting date, or empty for none.
        std::vector<Real> initial_margin_received, initial_margin_posted;
        /**
         * @brief Weights a_r and b_r, per reporting date, of the per-path sums
         *
         *   Σ_r a_r D max(V, 0)   and   Σ_r b_r D min(V, 0),
         *
         * whose means are integrals of the exposure profiles and whose
         * dispersion is their Monte-Carlo error, correlations between dates
         * included (with default probabilities times loss given default: CVA
         * and DVA). Empty for zeros.
         */
        std::vector<Real> positive_weights, negative_weights;
    };

    struct NettingSetGpuProfile
    {
        /// Per reporting date: statistics of D max(V, 0), D min(V, 0), D V.
        std::vector<ExposureDateStats> dates;
        /// `positive` and `negative`: the two weighted sums; `value` unused.
        ExposureDateStats weighted;
        int gpus = 0;
    };

    /// @throws GpuUnavailable without a device or on a CUDA error.
    NettingSetGpuProfile simulate_netting_set(const ExposureGpuRequest &request,
                                              const NettingSetGpuRequest &netting);

} // namespace quantModeling::gpu

#endif // GPU_EXPOSURE_HPP
