#ifndef GPU_EXPOSURE_HPP
#define GPU_EXPOSURE_HPP

#include <cstdint>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/xva/exposure_program.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral_path.hpp"
#include "quantModeling/risk/initial_margin_path.hpp"
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
        /// Logical blocks (2 048 paths) per launch; 0 takes what half of the
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
        /**
         * @brief Keep the distribution (lot G3, blueprint/wp/23-xva.md §20):
         *        every path's value V and discount weight at every reporting
         *        date stay on the device, each date is sorted there by value
         *        (Thrust), and the sorted pairs come back -- what a quantile
         *        (the PFE) is read from. One matrix of paths x dates for the
         *        netting set, not the cube's one per trade; the request is
         *        refused (GpuUnavailable) when even that does not fit.
         */
        bool distribution = false;
        /**
         * @brief The trades' contributions (lot G1, blueprint/wp/23-xva.md
         *        §20): 2K more netting sets of the same paths and terms,
         *        each trade alone (subset 2j) and the netting set without it
         *        (subset 2j + 1), with the weighted sums of each. The kernel
         *        values each trade once per date and nets every subset from
         *        those values, in the order of `trades`, as the host does.
         *        Needs no initial margin (a subset's margin is a model of its
         *        own: lot G8).
         */
        bool contributions = false;
        /// With `contributions` and `collateralised`: the balance after
        /// today's call of each of the 2K subsets, on its own value today.
        std::vector<Real> subset_held_today;
        /**
         * @brief Euler allocation of the positive sum (no collateral, no
         *        initial margin): per trade j, Σ_r a_r D v_j 1{V > 0}, V
         *        the netting set's value. The K sums add up to the netting
         *        set's, path by path.
         */
        bool euler = false;
        /**
         * @brief Running costs (#281 step 3, blueprint/wp/23-xva.md §20):
         *        weights c_r, d_r and e_r, per reporting date, of the
         *        per-path sums
         *
         *   Σ_r c_r D max(U, 0),   Σ_r d_r D min(U, 0)   and   Σ_r e_r D C,
         *
         * U the value after variation margin and before any initial margin
         * (segregated margin funds nothing), C the collateral held,
         * collateral_held(). With c = d = e = -rate S_C S_I Δt
         * (running_cost_weights()) and the borrowing spread, the lending
         * spread and r_c - r: FCA, FBA and ColVA path by path. Without
         * collateral U = V and C = 0. Each empty for zeros; all three empty
         * skip them.
         */
        std::vector<Real> funding_positive_weights, funding_negative_weights, collateral_weights;
        /**
         * @brief Initial margin per path (#281 step 4, lot G8): a dynamic
         *        initial margin fitted on the host, evaluated on each path
         *        from the netting set's value at the lagged date
         *        (initial_margin_at()), received and posted alike. One date
         *        per reporting date, on the same grid dates. Needs the
         *        collateral, and excludes the margin profiles above.
         */
        bool dynamic_margin = false;
        InitialMarginTable margin_model;
        /// With `dynamic_margin`: weights m_r of the per-path sum
        /// Σ_r m_r D IM -- with running_cost_weights() at FS_B - s_IM, the
        /// path's MVA. Empty for zeros.
        std::vector<Real> margin_weights;
    };

    /// One device's paths at one date, sorted by value.
    struct SortedExposures
    {
        std::vector<Real> value;  ///< V, undiscounted, ascending
        std::vector<Real> weight; ///< the paths' discount weights, in that order
    };

    struct NettingSetGpuProfile
    {
        /// With NettingSetGpuRequest::distribution: [device][reporting date].
        std::vector<std::vector<SortedExposures>> sorted;
        /// Per reporting date: statistics of D max(V, 0), D min(V, 0), D V.
        std::vector<ExposureDateStats> dates;
        /// `positive` and `negative`: the two weighted sums; `value` unused.
        ExposureDateStats weighted;
        /// With NettingSetGpuRequest::contributions, 2K: at 2j the weighted
        /// sums of trade j alone; at 2j + 1 the netting set's minus those of
        /// the netting set without it, path by path (the incremental sums:
        /// their error is that of the paired difference).
        std::vector<ExposureDateStats> subsets;
        /// With NettingSetGpuRequest::euler, K: `positive` is trade j's
        /// share of the positive weighted sum.
        std::vector<ExposureDateStats> euler;
        /// With the running-cost weights: `positive` and `negative` the two
        /// funding sums, `value` their sum path by path (the FVA, whose
        /// error is that of the pair, not the two added).
        ExposureDateStats funding;
        /// With the running-cost weights: `positive` is the collateral sum.
        ExposureDateStats collateral;
        /// With `dynamic_margin`, per reporting date: `positive` holds the
        /// statistics of D IM, the discounted margin.
        std::vector<ExposureDateStats> margin_dates;
        /// With `dynamic_margin`: `positive` is the margin sum (the MVA).
        ExposureDateStats margin;
        int gpus = 0;
    };

    /// @throws GpuUnavailable without a device or on a CUDA error.
    NettingSetGpuProfile simulate_netting_set(const ExposureGpuRequest &request,
                                              const NettingSetGpuRequest &netting);

} // namespace quantModeling::gpu

#endif // GPU_EXPOSURE_HPP
