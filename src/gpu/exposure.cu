#include "quantModeling/core/strided.hpp"
#include "quantModeling/gpu/exposure.hpp"
#include "quantModeling/utils/philox.hpp"

#include "logical_blocks.cuh"

#include <thrust/execution_policy.h>
#include <thrust/sort.h>

// The exposure engine on the device (blueprint/wp/23-xva.md §14.11, lot X7).
// One thread per path: it simulates the Hull-White state on the grid, then
// values every trade of the program at every date with the functions of
// engines/xva/exposure_program.hpp -- the ones the host runs.
//
// The per-path rows a kernel keeps on the device (state, netted value and
// flow, collateral, discounted exposure) are stored date-major: date i of the
// path in row slot `local` sits at `i * rows + local`, `rows` being the
// launch's capacity. A warp's 32 consecutive paths then read and write one
// contiguous line per date (Strided, core/strided.hpp). Path-major rows made
// half of the kernel's memory sectors wasted and its warps stall on L1TEX for
// half their cycles (Nsight Compute, V100, 2026-11-10).

namespace quantModeling::gpu
{

    namespace
    {
        using L = mc::LogicalBlocks;

        /// A launch never takes more than this many logical blocks, whatever
        /// the memory: its threads all hold the device until it returns.
        constexpr uint64_t kMaxBlocksPerLaunch = 256;

        /// Device memory, given back on scope exit. Lives and dies on the
        /// host thread that selected its device.
        template <class T>
        class DeviceArray
        {
          public:
            DeviceArray() = default;
            explicit DeviceArray(std::size_t count) { allocate(count); }
            explicit DeviceArray(const std::vector<T> &host)
            {
                if (host.empty())
                    return;
                allocate(host.size());
                detail::check(cudaMemcpy(ptr_, host.data(), host.size() * sizeof(T), cudaMemcpyHostToDevice),
                              "cudaMemcpy");
            }
            ~DeviceArray()
            {
                if (ptr_ != nullptr)
                    cudaFree(ptr_);
            }
            DeviceArray(const DeviceArray &) = delete;
            DeviceArray &operator=(const DeviceArray &) = delete;

            T *get() const { return ptr_; }

          private:
            void allocate(std::size_t count)
            {
                if (count > 0)
                    detail::check(cudaMalloc(&ptr_, count * sizeof(T)), "cudaMalloc");
            }

            T *ptr_ = nullptr;
        };

        /// The program's arrays on the device.
        struct DeviceProgram
        {
            explicit DeviceProgram(const xva::ExposureProgram &p)
                : trades(p.trades), dates(p.date_records), exps(p.exps), coupons(p.coupons),
                  accrued(p.accrued), paid(p.paid), option_dates(p.option_dates),
                  option_bonds(p.option_bonds), intervals(p.intervals),
                  n_trades(static_cast<int>(p.trades.size())), n_dates(static_cast<int>(p.dates))
            {
            }

            xva::ProgramView view() const
            {
                return {trades.get(),
                        dates.get(),
                        exps.get(),
                        coupons.get(),
                        accrued.get(),
                        paid.get(),
                        option_dates.get(),
                        option_bonds.get(),
                        intervals.get(),
                        n_trades,
                        n_dates};
            }

            DeviceArray<xva::ProgramTrade> trades;
            DeviceArray<xva::ProgramDate> dates;
            DeviceArray<xva::ProgramExp> exps;
            DeviceArray<xva::ProgramCoupon> coupons;
            DeviceArray<xva::ProgramAccrued> accrued;
            DeviceArray<int> paid;
            DeviceArray<xva::ProgramOptionDate> option_dates;
            DeviceArray<xva::ProgramOptionBond> option_bonds;
            DeviceArray<xva::ProgramInterval> intervals;
            int n_trades, n_dates;
        };

        struct DynamicsView
        {
            const Real *decay, *drift, *sd, *weight_scale, *weight_G;
        };

        struct DeviceDynamics
        {
            explicit DeviceDynamics(const xva::StateDynamics &d)
                : decay(d.decay), drift(d.drift), sd(d.sd), weight_scale(d.weight_scale), weight_G(d.weight_G)
            {
            }

            DynamicsView view() const
            {
                return {decay.get(), drift.get(), sd.get(), weight_scale.get(), weight_G.get()};
            }

            DeviceArray<Real> decay, drift, sd, weight_scale, weight_G;
        };

        /// The state of path `path` at every date: the recursion of the CPU
        /// engine, on the same draws.
        __device__ inline void simulate_state(const DynamicsView &d, int n, uint64_t seed, uint64_t path,
                                              const Strided<Real> &x)
        {
            PhiloxGaussianSource gaussian(seed, path);
            Real s = 0.0;
            for (int i = 0; i < n; ++i)
            {
                s = d.decay[i] * s + d.drift[i] + d.sd[i] * gaussian.next();
                x[i] = s;
            }
        }

        /// The state is date-major scratch (`rows` slots a date). The output
        /// matrices are (trade, path of the launch, date), the host cube's
        /// order: each trade's block of `count` rows goes to the host in one
        /// copy.
        __global__ void cube_kernel(xva::ProgramView p, DynamicsView d, uint64_t seed, uint64_t first_path,
                                    uint32_t count, size_t rows, Real *state, Real *weight, Real *values,
                                    Real *cashflows)
        {
            const uint32_t local = blockIdx.x * blockDim.x + threadIdx.x;
            if (local >= count)
                return;
            const int n = p.n_dates;
            const Strided<Real> x{state + local, rows};
            simulate_state(d, n, seed, first_path + local, x);
            Real *w = weight + static_cast<size_t>(local) * n;
            for (int i = 0; i < n; ++i)
                w[i] = d.weight_scale[i] * exp(d.weight_G[i] * x[i]);
            for (int k = 0; k < p.n_trades; ++k)
            {
                const xva::ProgramTrade &trade = p.trades[k];
                const bool exercised = xva::program_exercised(p, trade, x);
                const size_t row = (static_cast<size_t>(k) * count + local) * n;
                for (int i = 0; i < n; ++i)
                    values[row + i] = trade.quantity * xva::program_value(p, trade, i, x, exercised);
                if (cashflows != nullptr)
                    for (int i = 0; i < n; ++i)
                        cashflows[row + i] = trade.quantity * xva::program_cashflow(p, trade, i, x, exercised);
            }
        }

        struct NettingView
        {
            CollateralPlanView plan;
            const int *trades;
            int n_trades;
            bool collateralised;
            bool flows;
            const Real *im_received, *im_posted, *positive_weights, *negative_weights;
            /// Lot G1: the trades' values (and flows) kept per date, the 2K
            /// subsets' balances today, and which outputs are wanted.
            bool contributions;
            bool euler;
            const Real *subset_held_today;
            Real *trade_values; // K date-major blocks of n dates x rows, or null
            Real *trade_flows;  // the same for the flows, or null
            /// #281 step 3: the running costs' weights (each null for
            /// zeros), and whether their four sums are written.
            const Real *funding_positive_weights, *funding_negative_weights, *collateral_weights;
            bool running;
            /// #281 step 4: the dynamic initial margin, its per-date output
            /// (D IM, date-major like `discounted`) and the MVA's weights.
            bool dynamic_margin;
            InitialMarginView margin;
            Real *discounted_margin;
            const Real *margin_weights;
        };

        /// Scratch: four date-major blocks of n dates x `rows` slots (state,
        /// netted value, netted flow, collateral balance), then, with lot G1,
        /// one of m reporting dates (the discount weight). Out: the
        /// discounted collateralised value, date-major over the reporting
        /// dates, and the path's sums, sum-major (`rows` slots a sum).
        /// dist_value / dist_weight (or null): each path's undiscounted
        /// value and discount weight, date-major with `dist_rows` slots a
        /// reporting date, path `dist_first + local` of the device's share.
        __global__ void netting_kernel(xva::ProgramView p, DynamicsView d, NettingView ns, uint64_t seed,
                                       uint64_t first_path, uint32_t count, size_t rows, Real *scratch,
                                       Real *discounted, Real *weighted, Real *dist_value, Real *dist_weight,
                                       size_t dist_rows, size_t dist_first)
        {
            const uint32_t local = blockIdx.x * blockDim.x + threadIdx.x;
            if (local >= count)
                return;
            const int n = p.n_dates;
            const size_t block = static_cast<size_t>(n) * rows;
            const Strided<Real> x{scratch + local, rows}, value{x.base + block, rows}, flow{value.base + block, rows},
                held{flow.base + block, rows}, weight{held.base + block, rows};
            const bool per_trade = ns.trade_values != nullptr;
            const auto trade_row = [&](Real *base, int j) { return Strided<Real>{base + j * block + local, rows}; };
            simulate_state(d, n, seed, first_path + local, x);
            for (int i = 0; i < n; ++i)
            {
                value[i] = 0.0;
                flow[i] = 0.0;
            }
            // Close-out netting: the values add up before the positive part;
            // payment netting: the flows of a date settle for their sum.
            // __dadd_rn keeps the product apart from the sum (no FMA), so the
            // value kept for a trade is the one the netting set added.
            for (int j = 0; j < ns.n_trades; ++j)
            {
                const xva::ProgramTrade &trade = p.trades[ns.trades[j]];
                const bool exercised = xva::program_exercised(p, trade, x);
                for (int i = 0; i < n; ++i)
                {
                    const Real v = trade.quantity * xva::program_value(p, trade, i, x, exercised);
                    value[i] = __dadd_rn(value[i], v);
                    if (per_trade)
                        trade_row(ns.trade_values, j)[i] = v;
                }
                if (ns.flows)
                    for (int i = 0; i < n; ++i)
                    {
                        const Real f = trade.quantity * xva::program_cashflow(p, trade, i, x, exercised);
                        flow[i] = __dadd_rn(flow[i], f);
                        if (ns.trade_flows != nullptr)
                            trade_row(ns.trade_flows, j)[i] = f;
                    }
            }
            if (ns.collateralised)
                collateral_balances(ns.plan, value, held, ns.plan.held_today);

            const int m = ns.plan.reporting_dates;
            const Strided<Real> out{discounted + local, rows}, sums{weighted + local, rows};
            Real positive = 0.0, negative = 0.0;
            // The running costs: funding on U (after variation margin,
            // before initial margin), collateral on C.
            Real borrowed = 0.0, lent = 0.0, collateral = 0.0, margin_cost = 0.0;
            const Strided<Real> margin_out{ns.discounted_margin + local, rows};
            for (int r = 0; r < m; ++r)
            {
                const int i = ns.plan.reporting[r];
                const Real variation = ns.collateralised
                                           ? collateralised_value(ns.plan, r, value, flow, held, ns.plan.held_today)
                                           : value[i];
                Real exposure = variation;
                const Real w = d.weight_scale[i] * exp(d.weight_G[i] * x[i]);
                if (ns.dynamic_margin)
                {
                    // Received and posted alike, from the value when the
                    // last call was honoured.
                    const int lagged = ns.margin.lagged[r];
                    const Real im = initial_margin_at(ns.margin, r, lagged < 0 ? 0.0 : value[lagged]);
                    exposure = net_of_initial_margin(exposure, im, im);
                    margin_out[r] = w * im;
                    if (ns.margin_weights != nullptr)
                        margin_cost += ns.margin_weights[r] * (w * im);
                }
                else if (ns.im_received != nullptr || ns.im_posted != nullptr)
                    exposure = net_of_initial_margin(exposure, ns.im_received != nullptr ? ns.im_received[r] : 0.0,
                                                     ns.im_posted != nullptr ? ns.im_posted[r] : 0.0);
                if (ns.running)
                {
                    const Real u = w * variation;
                    if (ns.funding_positive_weights != nullptr && u > 0.0)
                        borrowed += ns.funding_positive_weights[r] * u;
                    if (ns.funding_negative_weights != nullptr && u < 0.0)
                        lent += ns.funding_negative_weights[r] * u;
                    if (ns.collateral_weights != nullptr && ns.collateralised)
                        collateral += ns.collateral_weights[r] * (w * collateral_held(ns.plan, r, held, ns.plan.held_today));
                }
                if (per_trade)
                    weight[r] = w;
                const Real v = w * exposure;
                out[r] = v;
                if (dist_value != nullptr)
                {
                    const size_t at = static_cast<size_t>(r) * dist_rows + dist_first + local;
                    dist_value[at] = exposure;
                    dist_weight[at] = w;
                }
                if (ns.positive_weights != nullptr && v > 0.0)
                    positive += ns.positive_weights[r] * v;
                if (ns.negative_weights != nullptr && v < 0.0)
                    negative += ns.negative_weights[r] * v;
            }
            sums[0] = positive;
            sums[1] = negative;
            if (ns.running)
            {
                // After the subsets' and the Euler shares' sums.
                const int at = 2 + (ns.contributions ? 4 * ns.n_trades : 0) + (ns.euler ? ns.n_trades : 0);
                sums[at] = borrowed;
                sums[at + 1] = lent;
                sums[at + 2] = borrowed + lent;
                sums[at + 3] = collateral;
            }
            if (ns.dynamic_margin)
                sums[2 + (ns.contributions ? 4 * ns.n_trades : 0) + (ns.euler ? ns.n_trades : 0) + (ns.running ? 4 : 0)] =
                    margin_cost;

            if (ns.euler)
            {
                // Each trade's share of the positive sum, on the dates where
                // the netting set's value is positive (no collateral here),
                // while `value` still holds the netting set.
                const int at = 2 + (ns.contributions ? 4 * ns.n_trades : 0);
                for (int j = 0; j < ns.n_trades; ++j)
                {
                    const Strided<Real> v = trade_row(ns.trade_values, j);
                    Real share = 0.0;
                    if (ns.positive_weights != nullptr)
                        for (int r = 0; r < m; ++r)
                        {
                            const int i = ns.plan.reporting[r];
                            if (value[i] > 0.0)
                                share += ns.positive_weights[r] * (weight[r] * v[i]);
                        }
                    sums[at + j] = share;
                }
            }

            if (ns.contributions)
            {
                // Subset 2j is trade j alone, 2j + 1 the others, netted in
                // the order of the netting set from the values kept above;
                // only the balance after today's call differs between them.
                // Lot G1, 2026-11-17, 10 trades under a CSA, 198 dates,
                // 50 000 paths: 0.093 s on one V100 (0.052 s on two) against
                // 4.23 s for the cube and xva_report() on 16 host threads.
                const int K = ns.n_trades;
                for (int s = 0; s < 2 * K; ++s)
                {
                    const int j = s / 2;
                    const bool alone = s % 2 == 0;
                    for (int i = 0; i < n; ++i)
                    {
                        value[i] = 0.0;
                        flow[i] = 0.0;
                    }
                    for (int k = 0; k < K; ++k)
                    {
                        if ((k == j) != alone)
                            continue;
                        const Strided<Real> v = trade_row(ns.trade_values, k);
                        for (int i = 0; i < n; ++i)
                            value[i] = __dadd_rn(value[i], v[i]);
                        if (ns.trade_flows != nullptr)
                        {
                            const Strided<Real> f = trade_row(ns.trade_flows, k);
                            for (int i = 0; i < n; ++i)
                                flow[i] = __dadd_rn(flow[i], f[i]);
                        }
                    }
                    const Real held_today = ns.collateralised ? ns.subset_held_today[s] : 0.0;
                    if (ns.collateralised)
                        collateral_balances(ns.plan, value, held, held_today);
                    Real pos = 0.0, neg = 0.0;
                    for (int r = 0; r < m; ++r)
                    {
                        const int i = ns.plan.reporting[r];
                        const Real exposure = ns.collateralised
                                                  ? collateralised_value(ns.plan, r, value, flow, held, held_today)
                                                  : value[i];
                        const Real v = weight[r] * exposure;
                        if (ns.positive_weights != nullptr && v > 0.0)
                            pos += ns.positive_weights[r] * v;
                        if (ns.negative_weights != nullptr && v < 0.0)
                            neg += ns.negative_weights[r] * v;
                    }
                    // Trade j alone; or the incremental sums, path by path.
                    sums[2 + 2 * s] = alone ? pos : positive - pos;
                    sums[2 + 2 * s + 1] = alone ? neg : negative - neg;
                }
            }
        }

        /// What the reduction adds for path u of a launch: its value at
        /// reporting date `segment`; past the last date, the path's sums --
        /// two by two (the netting set's, then each subset's), then one by
        /// one (Euler), then the funding sums three at a time and the
        /// collateral's alone.
        struct ReduceUnit
        {
            const Real *discounted; // date-major, `rows` slots a reporting date
            const Real *weighted;   // sum-major, `rows` slots a sum
            uint64_t m;
            uint64_t rows;
            uint64_t pairs;  // 1 + the subsets
            uint64_t shares; // the Euler shares
            bool running;    // the funding and collateral sums
            const Real *margin; // D IM, date-major like `discounted`, or null

            __device__ ExposureSample operator()(uint64_t segment, uint64_t u) const
            {
                if (segment < m)
                {
                    const Real v = discounted[segment * rows + u];
                    return {v > 0.0 ? v : 0.0, v < 0.0 ? v : 0.0, v};
                }
                uint64_t q = segment - m;
                if (q < pairs)
                    return {weighted[2 * q * rows + u], weighted[(2 * q + 1) * rows + u], 0.0};
                q -= pairs;
                if (q < shares)
                    return {weighted[(2 * pairs + q) * rows + u], 0.0, 0.0};
                q -= shares;
                uint64_t at = 2 * pairs + shares;
                if (running)
                {
                    if (q == 0)
                        return {weighted[at * rows + u], weighted[(at + 1) * rows + u], weighted[(at + 2) * rows + u]};
                    if (q == 1)
                        return {weighted[(at + 3) * rows + u], 0.0, 0.0};
                    q -= 2;
                    at += 4;
                }
                // The margin: D IM per reporting date, then the MVA's sum.
                if (q < m)
                    return {margin[q * rows + u], 0.0, 0.0};
                return {weighted[at * rows + u], 0.0, 0.0};
            }
        };

        void require(const ExposureGpuRequest &req)
        {
            if (req.program == nullptr || req.dynamics == nullptr)
                throw InvalidInput("exposure on GPU: no program or no dynamics");
            const std::size_t n = req.program->dates;
            const xva::StateDynamics &d = *req.dynamics;
            if (n == 0 || req.program->trades.empty() || req.paths == 0)
                throw InvalidInput("exposure on GPU: the simulation is empty");
            if (d.decay.size() != n || d.drift.size() != n || d.sd.size() != n || d.weight_scale.size() != n ||
                d.weight_G.size() != n)
                throw InvalidInput("exposure on GPU: the dynamics are not on the program's grid");
        }

        /// How many logical blocks one launch takes on `device`, from what is
        /// free there now: at most half (the assistant's LLM shares the card).
        uint64_t blocks_per_launch(int device, std::size_t bytes_per_path, uint64_t wanted, uint64_t cap)
        {
            const std::size_t per_block = bytes_per_path * L::kUnitsPerBlock;
            const std::size_t budget = free_memory(device) / 2;
            uint64_t blocks = budget / per_block;
            if (blocks == 0)
                throw GpuUnavailable("exposure on GPU: " + std::to_string(L::kUnitsPerBlock) + " paths need " + std::to_string(per_block >> 20) +
                                     " MiB on the device and half of what is free is " +
                                     std::to_string(budget >> 20) + " MiB; fewer trades or dates, or the CPU");
            blocks = std::min(blocks, kMaxBlocksPerLaunch);
            if (cap > 0)
                blocks = std::min(blocks, cap);
            return std::min(blocks, wanted);
        }

        /// Device k runs the k-th contiguous range of logical blocks on its
        /// own host thread: job(device, first, last). Unlike
        /// detail::run_on_devices a card joins from its first block on -- a
        /// path here is thousands of exponentials, not one.
        template <class Job>
        int share_blocks(const std::vector<int> &devices, uint64_t n_blocks, const Job &job)
        {
            const std::vector<int> devs = devices.empty() ? std::vector<int>{0} : devices;
            const std::size_t used = static_cast<std::size_t>(std::min<uint64_t>(devs.size(), n_blocks));
            if (used <= 1)
            {
                job(devs.front(), uint64_t{0}, n_blocks);
                return 1;
            }
            std::vector<std::exception_ptr> errors(used);
            std::vector<std::thread> threads;
            for (std::size_t k = 0; k < used; ++k)
            {
                const uint64_t first = n_blocks * k / used, last = n_blocks * (k + 1) / used;
                threads.emplace_back(
                    [&, k, first, last]()
                    {
                        try
                        {
                            job(devs[k], first, last);
                        }
                        catch (...)
                        {
                            errors[k] = std::current_exception();
                        }
                    });
            }
            for (std::thread &t : threads)
                t.join();
            for (const std::exception_ptr &e : errors)
                if (e)
                    std::rethrow_exception(e);
            return static_cast<int>(used);
        }

        unsigned grid_of(uint64_t count)
        {
            return static_cast<unsigned>((count + L::kThreadsPerBlock - 1) / L::kThreadsPerBlock);
        }
    } // namespace

    int simulate_exposure_cube(const ExposureGpuRequest &req, const ExposureCubeTarget &target)
    {
        require(req);
        const std::size_t n = req.program->dates, K = req.program->trades.size();
        const bool flows = !target.cashflows.empty();
        if (target.discount_weight == nullptr || target.values.size() != K || (flows && target.cashflows.size() != K))
            throw InvalidInput("exposure on GPU: the cube needs one matrix per trade");
        const uint64_t N = req.paths;

        const auto job = [&](int device, uint64_t first, uint64_t last)
        {
            detail::require_device(device);
            detail::check(cudaSetDevice(device), "cudaSetDevice");
            const DeviceProgram program(*req.program);
            const DeviceDynamics dynamics(*req.dynamics);
            // State, weight, and the values (and flows) of every trade.
            const std::size_t matrices = 2 + K * (flows ? 2 : 1);
            const uint64_t per_launch =
                blocks_per_launch(device, matrices * n * sizeof(Real), last - first, req.max_blocks_per_launch);
            const std::size_t rows = static_cast<std::size_t>(per_launch) * L::kUnitsPerBlock;
            const DeviceArray<Real> state(rows * n), weight(rows * n), values(K * rows * n),
                cashflows(flows ? K * rows * n : 0);
            for (uint64_t b = first; b < last; b += per_launch)
            {
                // Never past `last`: the blocks after it are another device's.
                const uint64_t launch_blocks = std::min(per_launch, last - b);
                const uint64_t first_path = b * L::kUnitsPerBlock;
                const uint64_t count = std::min(N, (b + launch_blocks) * L::kUnitsPerBlock) - first_path;
                cube_kernel<<<grid_of(count), L::kThreadsPerBlock>>>(program.view(), dynamics.view(), req.seed,
                                                                     first_path, static_cast<uint32_t>(count), rows,
                                                                     state.get(), weight.get(), values.get(),
                                                                     cashflows.get());
                detail::check(cudaGetLastError(), "kernel launch");
                const std::size_t cells = static_cast<std::size_t>(count) * n;
                const std::size_t at = static_cast<std::size_t>(first_path) * n;
                detail::check(cudaMemcpy(target.discount_weight + at, weight.get(), cells * sizeof(Real),
                                         cudaMemcpyDeviceToHost),
                              "cudaMemcpy");
                for (std::size_t k = 0; k < K; ++k)
                {
                    detail::check(cudaMemcpy(target.values[k] + at, values.get() + k * cells, cells * sizeof(Real),
                                             cudaMemcpyDeviceToHost),
                                  "cudaMemcpy");
                    if (flows)
                        detail::check(cudaMemcpy(target.cashflows[k] + at, cashflows.get() + k * cells,
                                                 cells * sizeof(Real), cudaMemcpyDeviceToHost),
                                      "cudaMemcpy");
                }
            }
        };
        return share_blocks(req.devices, L::count(N), job);
    }

    NettingSetGpuProfile simulate_netting_set(const ExposureGpuRequest &req, const NettingSetGpuRequest &netting)
    {
        require(req);
        const std::size_t n = req.program->dates, m = netting.plan.reporting.size();
        const CollateralPlan &plan = netting.plan;
        if (netting.trades.empty() || m == 0)
            throw InvalidInput("exposure on GPU: the netting set has no trade or no reporting date");
        for (const int k : netting.trades)
            if (k < 0 || static_cast<std::size_t>(k) >= req.program->trades.size())
                throw InvalidInput("exposure on GPU: trade index out of range");
        for (const int i : plan.reporting)
            if (i < 0 || static_cast<std::size_t>(i) >= n)
                throw InvalidInput("exposure on GPU: reporting date out of the grid");
        if (netting.collateralised && (plan.lagged.size() != n || plan.is_call_date.size() != n))
            throw InvalidInput("exposure on GPU: the collateral plan is not on the program's grid");
        const auto per_reporting_date = [m](const std::vector<Real> &v)
        { return v.empty() || v.size() == m; };
        if (!per_reporting_date(netting.initial_margin_received) ||
            !per_reporting_date(netting.initial_margin_posted) || !per_reporting_date(netting.positive_weights) ||
            !per_reporting_date(netting.negative_weights) || !per_reporting_date(netting.funding_positive_weights) ||
            !per_reporting_date(netting.funding_negative_weights) || !per_reporting_date(netting.collateral_weights))
            throw InvalidInput("exposure on GPU: margins and weights need one value per reporting date");
        const std::size_t K = netting.trades.size();
        const bool margined = !netting.initial_margin_received.empty() || !netting.initial_margin_posted.empty();
        const bool dynamic_margin = netting.dynamic_margin;
        if (dynamic_margin)
        {
            const InitialMarginTable &t = netting.margin_model;
            if (!netting.collateralised || margined)
                throw InvalidInput("exposure on GPU: a dynamic initial margin needs the collateral and replaces "
                                   "the margin profiles");
            if (t.dates() != m)
                throw InvalidInput("exposure on GPU: the margin model needs one date per reporting date");
            for (std::size_t r = 0; r < m; ++r)
                if (t.date[r] != plan.reporting[r] || t.lagged[r] >= t.date[r] ||
                    static_cast<std::size_t>(t.date[r]) >= n)
                    throw InvalidInput("exposure on GPU: the margin model is not on the reporting dates");
            if (!per_reporting_date(netting.margin_weights))
                throw InvalidInput("exposure on GPU: margin weights need one value per reporting date");
        }
        if ((netting.contributions || netting.euler) && (margined || dynamic_margin))
            throw InvalidInput("exposure on GPU: the trades' contributions under initial margin need each "
                               "subset's own margin model; use the cube");
        if (netting.euler && netting.collateralised)
            throw InvalidInput("exposure on GPU: no Euler allocation under a CSA (the exposure is not "
                               "homogeneous in the trades)");
        if (netting.contributions && netting.collateralised && netting.subset_held_today.size() != 2 * K)
            throw InvalidInput("exposure on GPU: the contributions need the balance today of each of the 2K "
                               "subsets");
        const bool per_trade = netting.contributions || netting.euler;
        const bool trade_flows = netting.contributions && netting.collateralised && plan.needs_cashflows();
        const std::size_t subsets = netting.contributions ? 2 * K : 0, shares = netting.euler ? K : 0;
        const bool running = !netting.funding_positive_weights.empty() ||
                             !netting.funding_negative_weights.empty() || !netting.collateral_weights.empty();
        const std::size_t sums = 2 + 2 * subsets + shares + (running ? 4 : 0) + (dynamic_margin ? 1 : 0);

        const uint64_t N = req.paths;
        const uint64_t n_blocks = L::count(N);
        // Partials of (logical block, segment): the reporting dates, the
        // netting set's weighted sums, each subset's, each trade's share,
        // the funding sums and the collateral's.
        // and, with the dynamic margin, D IM per date and the MVA's sum.
        const std::size_t segments = m + 1 + subsets + shares + (running ? 2 : 0) + (dynamic_margin ? m + 1 : 0);
        std::vector<ExposureDateStats> partials(static_cast<std::size_t>(n_blocks) * segments);

        // The sorted distributions, one slot per device (in block order).
        const std::size_t n_devices = std::max<std::size_t>(req.devices.size(), 1);
        std::vector<std::vector<SortedExposures>> sorted(netting.distribution ? n_devices : 0);
        std::vector<uint64_t> device_first(n_devices, 0);
        for (std::size_t k = 0; k < n_devices; ++k)
            device_first[k] = n_blocks * k / std::min<uint64_t>(n_devices, std::max<uint64_t>(n_blocks, 1));

        const auto job = [&](int device, uint64_t first, uint64_t last)
        {
            detail::require_device(device);
            detail::check(cudaSetDevice(device), "cudaSetDevice");
            const DeviceProgram program(*req.program);
            const DeviceDynamics dynamics(*req.dynamics);
            // This device's whole share of the paths, kept for the sort.
            const std::size_t share = static_cast<std::size_t>(std::min(N, last * L::kUnitsPerBlock) -
                                                                first * L::kUnitsPerBlock);
            if (netting.distribution)
            {
                const std::size_t need = 2 * share * m * sizeof(Real);
                // the sort's own scratch is as large again
                if (2 * need > free_memory(device) / 2)
                    throw GpuUnavailable("exposure on GPU: the distribution of " + std::to_string(share) +
                                         " paths over " + std::to_string(m) + " dates needs " +
                                         std::to_string((2 * need) >> 20) +
                                         " MiB on the device, more than half of what is free: fewer paths, "
                                         "or the CPU");
            }
            const DeviceArray<Real> dist_value(netting.distribution ? share * m : 0),
                dist_weight(netting.distribution ? share * m : 0);
            const DeviceArray<int> trades(netting.trades), lagged(plan.lagged), reporting(plan.reporting);
            const DeviceArray<unsigned char> is_call_date(plan.is_call_date);
            const DeviceArray<Real> im_received(netting.initial_margin_received),
                im_posted(netting.initial_margin_posted), positive_weights(netting.positive_weights),
                negative_weights(netting.negative_weights), subset_held_today(netting.subset_held_today),
                funding_positive_weights(netting.funding_positive_weights),
                funding_negative_weights(netting.funding_negative_weights),
                collateral_weights(netting.collateral_weights), margin_weights(netting.margin_weights);
            const InitialMarginTable &mt = netting.margin_model;
            const DeviceArray<int> m_lagged(mt.lagged), m_mean_first(mt.mean_first), m_mean_buckets(mt.mean_buckets),
                m_variance_first(mt.variance_first), m_variance_buckets(mt.variance_buckets), m_degree(mt.degree),
                m_term_first(mt.term_first), m_terms(mt.terms), m_power(mt.power);
            const DeviceArray<Real> m_average(mt.average_variance), m_edge(mt.edge), m_center(mt.center),
                m_scale(mt.scale), m_beta(mt.beta);

            NettingView ns;
            ns.plan = view(plan);
            ns.plan.lagged = lagged.get();
            ns.plan.is_call_date = is_call_date.get();
            ns.plan.reporting = reporting.get();
            ns.plan.dates = static_cast<int>(n);
            ns.trades = trades.get();
            ns.n_trades = static_cast<int>(netting.trades.size());
            ns.collateralised = netting.collateralised;
            ns.flows = netting.collateralised && plan.needs_cashflows();
            ns.im_received = im_received.get();
            ns.im_posted = im_posted.get();
            ns.positive_weights = positive_weights.get();
            ns.negative_weights = negative_weights.get();
            ns.contributions = netting.contributions;
            ns.euler = netting.euler;
            ns.subset_held_today = subset_held_today.get();
            ns.funding_positive_weights = funding_positive_weights.get();
            ns.funding_negative_weights = funding_negative_weights.get();
            ns.collateral_weights = collateral_weights.get();
            ns.running = running;
            ns.dynamic_margin = dynamic_margin;
            ns.margin = {m_lagged.get(),   m_mean_first.get(), m_mean_buckets.get(), m_variance_first.get(),
                         m_variance_buckets.get(), m_average.get(), m_edge.get(), m_center.get(),
                         m_scale.get(),    m_degree.get(),     m_term_first.get(),   m_terms.get(),
                         m_power.get(),    m_beta.get(),       mt.today,             mt.scaling,
                         mt.quantile};
            ns.margin_weights = margin_weights.get();

            // Per path: the four blocks of n dates, the discount weight of
            // each reporting date and each trade's values (and flows) with
            // lot G1, the discounted values, the sums.
            const std::size_t trade_cells = per_trade ? K * n * (trade_flows ? 2 : 1) : 0;
            const std::size_t scratch_cells = 4 * n + (per_trade ? m : 0) + trade_cells;
            const std::size_t bytes_per_path = (scratch_cells + m * (dynamic_margin ? 2 : 1) + sums) * sizeof(Real);
            const uint64_t per_launch =
                blocks_per_launch(device, bytes_per_path, last - first, req.max_blocks_per_launch);
            const std::size_t rows = static_cast<std::size_t>(per_launch) * L::kUnitsPerBlock;
            const DeviceArray<Real> scratch(rows * scratch_cells), discounted(rows * m), weighted(rows * sums),
                discounted_margin(dynamic_margin ? rows * m : 0);
            ns.discounted_margin = discounted_margin.get();
            // The trades' blocks follow the five of the kernel's scratch.
            Real *const trades_base = scratch.get() + rows * (4 * n + (per_trade ? m : 0));
            ns.trade_values = per_trade ? trades_base : nullptr;
            ns.trade_flows = trade_flows ? trades_base + rows * K * n : nullptr;
            const ReduceUnit unit{discounted.get(), weighted.get(), m,       rows,
                                  1 + subsets,     shares,         running, discounted_margin.get()};
            for (uint64_t b = first; b < last; b += per_launch)
            {
                const uint64_t launch_blocks = std::min(per_launch, last - b);
                const uint64_t first_path = b * L::kUnitsPerBlock;
                const uint64_t count = std::min(N, (b + launch_blocks) * L::kUnitsPerBlock) - first_path;
                netting_kernel<<<grid_of(count), L::kThreadsPerBlock>>>(program.view(), dynamics.view(), ns,
                                                                        req.seed, first_path,
                                                                        static_cast<uint32_t>(count), rows, scratch.get(),
                                                                        discounted.get(), weighted.get(),
                                                                        dist_value.get(), dist_weight.get(), share,
                                                                        static_cast<std::size_t>(
                                                                            first_path - first * L::kUnitsPerBlock));
                detail::check(cudaGetLastError(), "kernel launch");
                // The paths of the launch are `launch_blocks` whole logical
                // blocks (the last one of the run may be short): reducing
                // them as a run of their own gives each block the partial it
                // has in the whole run.
                const std::vector<ExposureDateStats> reduced = detail::run_block_range<ExposureDateStats>(
                    device, 0, segments * launch_blocks, launch_blocks, count, unit, 0);
                for (std::size_t s = 0; s < segments; ++s)
                    for (uint64_t j = 0; j < launch_blocks; ++j)
                        partials[static_cast<std::size_t>(b + j) * segments + s] =
                            reduced[s * static_cast<std::size_t>(launch_blocks) + static_cast<std::size_t>(j)];
            }
            if (netting.distribution)
            {
                // Each date sorted by value on the device, its weights
                // carried along (a radix sort: deterministic), then home.
                std::size_t slot = 0;
                while (slot + 1 < n_devices && device_first[slot] != first)
                    ++slot;
                std::vector<SortedExposures> &out = sorted[slot];
                out.resize(m);
                for (std::size_t r = 0; r < m; ++r)
                {
                    Real *v = dist_value.get() + r * share, *w = dist_weight.get() + r * share;
                    thrust::sort_by_key(thrust::device, v, v + share, w);
                    out[r].value.resize(share);
                    out[r].weight.resize(share);
                    detail::check(cudaMemcpy(out[r].value.data(), v, share * sizeof(Real), cudaMemcpyDeviceToHost),
                                  "cudaMemcpy");
                    detail::check(cudaMemcpy(out[r].weight.data(), w, share * sizeof(Real), cudaMemcpyDeviceToHost),
                                  "cudaMemcpy");
                }
            }
        };

        NettingSetGpuProfile out;
        out.gpus = share_blocks(req.devices, n_blocks, job);
        // The host folds the partials in block order, date by date.
        const auto fold = [&](std::size_t s)
        {
            ExposureDateStats total;
            for (uint64_t b = 0; b < n_blocks; ++b)
                total.merge(partials[static_cast<std::size_t>(b) * segments + s]);
            return total;
        };
        out.dates.reserve(m);
        for (std::size_t s = 0; s < m; ++s)
            out.dates.push_back(fold(s));
        out.weighted = fold(m);
        for (std::size_t q = 0; q < subsets; ++q)
            out.subsets.push_back(fold(m + 1 + q));
        for (std::size_t j = 0; j < shares; ++j)
            out.euler.push_back(fold(m + 1 + subsets + j));
        if (running)
        {
            out.funding = fold(m + 1 + subsets + shares);
            out.collateral = fold(m + 1 + subsets + shares + 1);
        }
        if (dynamic_margin)
        {
            const std::size_t first = m + 1 + subsets + shares + (running ? 2 : 0);
            for (std::size_t r = 0; r < m; ++r)
                out.margin_dates.push_back(fold(first + r));
            out.margin = fold(first + m);
        }
        out.sorted = std::move(sorted);
        return out;
    }

} // namespace quantModeling::gpu
