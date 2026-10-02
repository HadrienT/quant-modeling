#include "quantModeling/gpu/exposure.hpp"
#include "quantModeling/utils/philox.hpp"

#include "logical_blocks.cuh"

// The exposure engine on the device (blueprint/wp/23-xva.md §14.11, lot X7).
// One thread per path: it simulates the Hull-White state on the grid, then
// values every trade of the program at every date with the functions of
// engines/xva/exposure_program.hpp -- the ones the host runs.

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
        __device__ inline void simulate_state(const DynamicsView &d, int n, uint64_t seed, uint64_t path, Real *x)
        {
            PhiloxGaussianSource gaussian(seed, path);
            Real s = 0.0;
            for (int i = 0; i < n; ++i)
            {
                s = d.decay[i] * s + d.drift[i] + d.sd[i] * gaussian.next();
                x[i] = s;
            }
        }

        /// Matrices are (trade, path of the launch, date): each trade's block
        /// of `count` rows goes to the host in one copy.
        __global__ void cube_kernel(xva::ProgramView p, DynamicsView d, uint64_t seed, uint64_t first_path,
                                    uint32_t count, Real *state, Real *weight, Real *values, Real *cashflows)
        {
            const uint32_t local = blockIdx.x * blockDim.x + threadIdx.x;
            if (local >= count)
                return;
            const int n = p.n_dates;
            Real *x = state + static_cast<size_t>(local) * n;
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
        };

        /// Scratch: four rows of n per path (state, netted value, netted
        /// flow, collateral balance). Out: the discounted collateralised
        /// value per reporting date, and the two weighted sums of the path.
        __global__ void netting_kernel(xva::ProgramView p, DynamicsView d, NettingView ns, uint64_t seed,
                                       uint64_t first_path, uint32_t count, Real *scratch, Real *discounted,
                                       Real *weighted)
        {
            const uint32_t local = blockIdx.x * blockDim.x + threadIdx.x;
            if (local >= count)
                return;
            const int n = p.n_dates;
            Real *x = scratch + static_cast<size_t>(local) * 4 * n;
            Real *value = x + n, *flow = value + n, *held = flow + n;
            simulate_state(d, n, seed, first_path + local, x);
            for (int i = 0; i < n; ++i)
            {
                value[i] = 0.0;
                flow[i] = 0.0;
            }
            // Close-out netting: the values add up before the positive part;
            // payment netting: the flows of a date settle for their sum.
            for (int j = 0; j < ns.n_trades; ++j)
            {
                const xva::ProgramTrade &trade = p.trades[ns.trades[j]];
                const bool exercised = xva::program_exercised(p, trade, x);
                for (int i = 0; i < n; ++i)
                    value[i] += trade.quantity * xva::program_value(p, trade, i, x, exercised);
                if (ns.flows)
                    for (int i = 0; i < n; ++i)
                        flow[i] += trade.quantity * xva::program_cashflow(p, trade, i, x, exercised);
            }
            if (ns.collateralised)
                collateral_balances(ns.plan, value, held, ns.plan.held_today);

            const int m = ns.plan.reporting_dates;
            Real *out = discounted + static_cast<size_t>(local) * m;
            Real positive = 0.0, negative = 0.0;
            for (int r = 0; r < m; ++r)
            {
                const int i = ns.plan.reporting[r];
                Real exposure = ns.collateralised
                                    ? collateralised_value(ns.plan, r, value, flow, held, ns.plan.held_today)
                                    : value[i];
                if (ns.im_received != nullptr || ns.im_posted != nullptr)
                    exposure = net_of_initial_margin(exposure, ns.im_received != nullptr ? ns.im_received[r] : 0.0,
                                                     ns.im_posted != nullptr ? ns.im_posted[r] : 0.0);
                const Real v = d.weight_scale[i] * exp(d.weight_G[i] * x[i]) * exposure;
                out[r] = v;
                if (ns.positive_weights != nullptr && v > 0.0)
                    positive += ns.positive_weights[r] * v;
                if (ns.negative_weights != nullptr && v < 0.0)
                    negative += ns.negative_weights[r] * v;
            }
            weighted[2 * static_cast<size_t>(local)] = positive;
            weighted[2 * static_cast<size_t>(local) + 1] = negative;
        }

        /// What the reduction adds for path u of a launch: its value at
        /// reporting date `segment`, or, for the segment after the last
        /// date, its two weighted sums.
        struct ReduceUnit
        {
            const Real *discounted;
            const Real *weighted;
            uint64_t m;

            __device__ ExposureSample operator()(uint64_t segment, uint64_t u) const
            {
                if (segment >= m)
                    return {weighted[2 * u], weighted[2 * u + 1], 0.0};
                const Real v = discounted[u * m + segment];
                return {v > 0.0 ? v : 0.0, v < 0.0 ? v : 0.0, v};
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
                throw GpuUnavailable("exposure on GPU: 4096 paths need " + std::to_string(per_block >> 20) +
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
                const uint64_t first_path = b * L::kUnitsPerBlock;
                const uint64_t count = std::min(N, (b + per_launch) * L::kUnitsPerBlock) - first_path;
                cube_kernel<<<grid_of(count), L::kThreadsPerBlock>>>(program.view(), dynamics.view(), req.seed,
                                                                     first_path, static_cast<uint32_t>(count),
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
            !per_reporting_date(netting.negative_weights))
            throw InvalidInput("exposure on GPU: margins and weights need one value per reporting date");

        const uint64_t N = req.paths;
        const uint64_t n_blocks = L::count(N);
        // Partials of (logical block, segment): the reporting dates, then the
        // weighted sums.
        const std::size_t segments = m + 1;
        std::vector<ExposureDateStats> partials(static_cast<std::size_t>(n_blocks) * segments);

        const auto job = [&](int device, uint64_t first, uint64_t last)
        {
            detail::require_device(device);
            detail::check(cudaSetDevice(device), "cudaSetDevice");
            const DeviceProgram program(*req.program);
            const DeviceDynamics dynamics(*req.dynamics);
            const DeviceArray<int> trades(netting.trades), lagged(plan.lagged), reporting(plan.reporting);
            const DeviceArray<unsigned char> is_call_date(plan.is_call_date);
            const DeviceArray<Real> im_received(netting.initial_margin_received),
                im_posted(netting.initial_margin_posted), positive_weights(netting.positive_weights),
                negative_weights(netting.negative_weights);

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

            const std::size_t bytes_per_path = (4 * n + m + 2) * sizeof(Real);
            const uint64_t per_launch =
                blocks_per_launch(device, bytes_per_path, last - first, req.max_blocks_per_launch);
            const std::size_t rows = static_cast<std::size_t>(per_launch) * L::kUnitsPerBlock;
            const DeviceArray<Real> scratch(rows * 4 * n), discounted(rows * m), weighted(rows * 2);
            const ReduceUnit unit{discounted.get(), weighted.get(), m};
            for (uint64_t b = first; b < last; b += per_launch)
            {
                const uint64_t launch_blocks = std::min(per_launch, last - b);
                const uint64_t first_path = b * L::kUnitsPerBlock;
                const uint64_t count = std::min(N, (b + launch_blocks) * L::kUnitsPerBlock) - first_path;
                netting_kernel<<<grid_of(count), L::kThreadsPerBlock>>>(program.view(), dynamics.view(), ns,
                                                                        req.seed, first_path,
                                                                        static_cast<uint32_t>(count), scratch.get(),
                                                                        discounted.get(), weighted.get());
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
        return out;
    }

} // namespace quantModeling::gpu
