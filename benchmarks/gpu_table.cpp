// The benchmark of blueprint/wp/19-gpu.md §9 (lot G4): wall time to reach a
// target standard error, CPU 1 thread / CPU n threads / 1 V100 / 2 V100, in
// pseudo-random (Philox) and Sobol RQMC with the Brownian bridge.
//
//   build-cuda/qm_gpu_table_bench [cpu_threads=8] [cpu_budget_s=30]
//
// Rows:
//   - Vanilla BS: the ATM call of lot G0, the dedicated kernel (six
//     estimators per path);
//   - Up-and-out, daily, local vol: the library script, a skewed surface,
//     252 steps a year;
//   - Worst-of autocall, 3 assets: the library script, correlated
//     Black-Scholes;
//   - Superbucket: the local-vol adjoint of a one-year call on the 30 x 12
//     Dupire grid of the market-vega endpoint (363 risks), daily steps.
// Target: 1e-4 relative standard error on the price (1e-3 on the delta for
// the superbucket, whose unit of work is a path *and* 363 derivatives).
//
// Every column computes the same thing: the CPU columns run the kernels' own
// per-path code (engines/mc/script_path.hpp, script_adjoint.hpp,
// kernels/vanilla_bs.hpp) on the host, over the same logical blocks -- so the
// comparison is of the hardware, not of two implementations. Pseudo-random:
// a pilot fixes the path count n = Var / (target x price)^2. Sobol: the
// path count doubles until the replicates' spread meets the target (capped;
// the achieved error is printed). A run -- CPU or GPU -- that would exceed
// the budget is timed on a fraction of the work (at least a quarter of the
// budget, so fixed costs are negligible) and scaled linearly: marked "*".
// Pseudo-random rows are sized by the pilot for the target error; their
// "Rel. error" column is that target.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/script_path_host.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/thread_pool.hpp"

namespace qm = quantModeling;
using qm::Real;

namespace
{
    using Clock = std::chrono::steady_clock;

    double seconds(const std::function<void()> &f)
    {
        const auto t0 = Clock::now();
        f();
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    struct Cell
    {
        double s = 0.0;
        bool extrapolated = false;
    };

    std::string fmt(const Cell &c)
    {
        char buf[64];
        if (c.s <= 0.0)
            return "n/a";
        if (c.s < 1.0)
            std::snprintf(buf, sizeof buf, "%.0f ms%s", 1e3 * c.s, c.extrapolated ? " *" : "");
        else if (c.s < 600.0)
            std::snprintf(buf, sizeof buf, "%.1f s%s", c.s, c.extrapolated ? " *" : "");
        else
            std::snprintf(buf, sizeof buf, "%.0f min%s", c.s / 60.0, c.extrapolated ? " *" : "");
        return buf;
    }

    /// Time `run(fraction)` -- which does `fraction` of the work -- in full
    /// when a small probe says it fits the budget, else on a fraction.
    Cell budgeted(double budget, const std::function<void(double)> &run)
    {
        // Probe on growing fractions until one takes half a second.
        double fraction = 1.0 / 4096.0, probe = 0.0;
        for (;; fraction = std::min(1.0, fraction * 8.0))
        {
            probe = seconds([&]
                            { run(fraction); });
            if (probe >= 0.5 || fraction >= 1.0)
                break;
        }
        if (fraction >= 1.0)
            return {probe, false};
        const double predicted = probe / fraction;
        if (predicted <= budget)
            return {seconds([&]
                            { run(1.0); }),
                    false};
        const double f = std::max(fraction, std::min(1.0, budget / 4.0 / predicted));
        return {seconds([&]
                        { run(f); }) /
                    f,
                true};
    }

    /// run(b) for b < n: in parallel on the pool (a task per replicate --
    /// a replicate of a small run is a block or two, too few to share), or
    /// in sequence without one.
    void for_replicates(int n, qm::ThreadPool *pool, const std::function<void(int)> &run)
    {
        if (!pool)
        {
            for (int b = 0; b < n; ++b)
                run(b);
            return;
        }
        std::vector<qm::TaskHandle> handles;
        for (int b = 0; b < n; ++b)
            handles.push_back(pool->spawn_task([&run, b]()
                                               {
                run(b);
                return true; }));
        for (auto &h : handles)
            pool->active_wait(h);
    }

    std::string read_script(const std::string &name)
    {
        std::ifstream in(std::filesystem::path(QM_PRODUCT_LIBRARY_DIR) / (name + ".qms"));
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    const qm::ValuationContext kCtx{qm::Date::from_iso("2026-06-01")};

    // The market-vega endpoint's grid: 30 strikes x 12 maturities.
    std::vector<Real> strikes()
    {
        std::vector<Real> k;
        for (int i = 0; i < 30; ++i)
            k.push_back(70.0 + 70.0 * i / 29.0);
        return k;
    }
    std::vector<Real> maturities()
    {
        std::vector<Real> t;
        for (int j = 0; j < 12; ++j)
            t.push_back(0.1 + 2.9 * j / 11.0);
        return t;
    }
    std::vector<Real> skew()
    {
        std::vector<Real> g;
        for (Real k : strikes())
            for (Real t : maturities())
                g.push_back(0.2 + 0.25 * (100.0 - k) / 100.0 + 0.01 * t);
        return g;
    }

    /// A script and its model as the device sees them, on the host.
    struct HostScript
    {
        std::unique_ptr<qm::ScriptedProduct<Real>> product;
        std::unique_ptr<qm::ISimulationModel<Real>> model;
        qm::DeviceModel dm;
        std::vector<std::vector<qm::Time>> mats;
        std::vector<Real> baseline;
        std::unique_ptr<qm::mc::ScriptPathHost> host;

        HostScript(const std::string &source, std::unique_ptr<qm::ISimulationModel<Real>> m)
            : product(std::make_unique<qm::ScriptedProduct<Real>>(source, kCtx)), model(std::move(m))
        {
            model->init(product->timeline(), product->defline());
            if (!model->describe_device(dm))
                throw std::runtime_error("model has no device description");
            for (const qm::SampleDef &d : product->defline())
                mats.push_back(d.discount_mats);
            baseline = product->baseline();
            host = std::make_unique<qm::mc::ScriptPathHost>(product->program(), dm, mats, baseline);
        }
    };

    /// The kernel's per-path code on the host: `units` units of each
    /// replicate (Philox antithetic pairs, or Sobol points of `tables`).
    qm::WelfordAccumulator cpu_script(const HostScript &h, uint64_t seed, uint64_t units,
                                      const qm::mc::SobolTables *tables, qm::ThreadPool *pool)
    {
        const qm::mc::ScriptPathView &v = h.host->view;
        const auto in = qm::mc::script_inputs<double>(v);
        if (!tables)
        {
            auto fn = [&](uint64_t u)
            {
                qm::mc::PathDraws d;
                d.seed = seed;
                d.stride = v.stride;
                d.begin(u);
                qm::mc::PathDraws d0 = d;
                const double a = qm::mc::script_path<double>(v, in, d, 1.0, nullptr);
                const double b = qm::mc::script_path<double>(v, in, d0, -1.0, nullptr);
                return 0.5 * (a + b);
            };
            return qm::mc::reduce_logical_blocks<qm::WelfordAccumulator>(units, fn, pool);
        }
        const qm::mc::SobolBridgeView sv = tables->view();
        std::vector<double> means(static_cast<std::size_t>(tables->replicates));
        for_replicates(tables->replicates, pool, [&](int b)
                       {
            auto fn = [&](uint64_t u)
            {
                thread_local std::vector<double> pt, out;
                pt.resize(static_cast<std::size_t>(sv.dim));
                out.resize(static_cast<std::size_t>(sv.dim));
                qm::mc::sobol_bridged_gaussians(sv, static_cast<uint64_t>(b), static_cast<uint32_t>(u), pt.data(), 1,
                                                out.data(), 1);
                qm::mc::PathDraws d;
                d.stride = v.stride;
                d.given = out.data();
                d.begin(u);
                return qm::mc::script_path<double>(v, in, d, 1.0, nullptr);
            };
            means[static_cast<std::size_t>(b)] = qm::mc::reduce_logical_blocks<qm::WelfordAccumulator>(units, fn).mean; });
        qm::WelfordAccumulator reps;
        for (double x : means)
            reps.add(x);
        return reps;
    }

    struct Row
    {
        std::string name;
        Cell cpu1, cpuN, gpu1, gpu2;
        long long paths = 0;
        double rel_se = 0.0;
    };

    void print(const std::vector<Row> &rows, const char *title, int threads)
    {
        std::printf("\n%s\n\n| | Paths | Rel. error | CPU 1 thread | CPU %d threads | 1 V100 | 2 V100 |\n"
                    "|---|---|---|---|---|---|---|\n",
                    title, threads);
        for (const Row &r : rows)
            std::printf("| %s | %.3g | %.1e | %s | %s | %s | %s |\n", r.name.c_str(), static_cast<double>(r.paths),
                        r.rel_se, fmt(r.cpu1).c_str(), fmt(r.cpuN).c_str(), fmt(r.gpu1).c_str(),
                        fmt(r.gpu2).c_str());
    }
} // namespace

int main(int argc, char **argv)
{
    const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    const double budget = argc > 2 ? std::atof(argv[2]) : 30.0;
    const double target = 1e-4, target_risk = 1e-3;
    if (qm::gpu::device_count() < 2)
    {
        std::fprintf(stderr, "needs two CUDA devices\n");
        return 1;
    }
    for (int d = 0; d < 2; ++d)
        qm::gpu::warm_up(d);
    qm::ThreadPool pool;
    pool.start(static_cast<std::size_t>(threads - 1)); // the caller is the last thread

    std::vector<Row> pseudo, sobol;

    // ── Vanilla BS: the dedicated kernel of lot G0 ─────────────────────────
    {
        qm::mc::VanillaTerminalSpec s;
        const double S0 = 100, K = 100, r = 0.05, q = 0.02, sigma = 0.20, T = 1.0, dT = 1.0 / 365.0;
        const double mu = r - q - 0.5 * sigma * sigma;
        s.S0 = S0;
        s.K = K;
        s.sigma = sigma;
        s.T = T;
        s.sqrtT = 1.0;
        s.movedSpot = S0 * std::exp(mu * T);
        s.rootVariance = sigma;
        s.df = std::exp(-r * T);
        s.dS = 0.01 * S0;
        s.factor_up = 1.01;
        s.factor_dn = 0.99;
        s.theta_bump = dT;
        s.movedSpot_upT = S0 * std::exp(mu * (T + dT));
        s.movedSpot_dnT = S0 * std::exp(mu * (T - dT));
        s.rootVariance_upT = sigma * std::sqrt(T + dT);
        s.rootVariance_dnT = sigma * std::sqrt(T - dT);
        s.df_upT = std::exp(-r * (T + dT));
        s.df_dnT = std::exp(-r * (T - dT));

        using Unit = qm::mc::VanillaPhiloxUnit<qm::OptionType::Call, true, false>;
        const Unit unit{s, 20260926, 0.0};
        const auto pilot = qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(uint64_t{1} << 20, unit, &pool);
        const double rel_sd = std::sqrt(pilot.payoff.variance()) / pilot.payoff.mean;
        const auto n = static_cast<uint64_t>(std::ceil(rel_sd * rel_sd / (target * target)));
        Row row{"Vanilla BS"};
        row.paths = 2 * static_cast<long long>(n);
        row.cpu1 = budgeted(budget, [&](double f)
                            { qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(static_cast<uint64_t>(f * n), unit); });
        row.cpuN = budgeted(budget, [&](double f)
                            { qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(static_cast<uint64_t>(f * n), unit, &pool); });
        qm::gpu::VanillaGpuRequest req;
        req.spec = s;
        req.n_units = n;
        req.seed = unit.seed;
        qm::mc::VanillaStats g;
        for (int k : {1, 2})
        {
            req.devices = k == 1 ? std::vector<int>{0} : std::vector<int>{0, 1};
            (k == 1 ? row.gpu1 : row.gpu2).s = seconds([&]
                                                       { g = qm::gpu::simulate_vanilla_terminal(req); });
        }
        row.rel_se = g.payoff.std_error() / g.payoff.mean;
        pseudo.push_back(row);

        // Sobol: 16 replicates of m points, m doubling to the target.
        Row srow{"Vanilla BS"};
        uint64_t m = uint64_t{1} << 12;
        std::vector<qm::SobolSequence> seqs;
        for (int b = 0; b < 16; ++b)
            seqs.emplace_back(1, (static_cast<uint64_t>(20260926u) << 32) | static_cast<uint64_t>(b));
        auto run_gpu = [&](uint64_t points, std::vector<int> devs)
        {
            qm::WelfordAccumulator reps;
            for (const auto &seq : seqs)
            {
                qm::gpu::VanillaSobolGpuRequest sr;
                sr.spec = s;
                std::copy_n(seq.directions().begin(), 32, sr.directions);
                sr.shift = seq.shifts()[0];
                sr.n_points = points;
                sr.devices = devs;
                reps.add(qm::gpu::simulate_vanilla_sobol(sr).payoff.mean);
            }
            return reps;
        };
        qm::WelfordAccumulator reps;
        for (;; m *= 2)
        {
            reps = run_gpu(m, {0, 1});
            if (reps.std_error() / reps.mean <= target || m >= (uint64_t{1} << 26))
                break;
        }
        srow.paths = static_cast<long long>(16 * m);
        srow.rel_se = reps.std_error() / reps.mean;
        auto cpu_sobol = [&](double f, qm::ThreadPool *p)
        {
            for_replicates(16, p, [&](int b)
                           {
                const auto &seq = seqs[static_cast<std::size_t>(b)];
                qm::mc::VanillaSobolUnit<qm::OptionType::Call, false> su;
                su.spec = s;
                std::copy_n(seq.directions().begin(), 32, su.V);
                su.shift = seq.shifts()[0];
                qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(static_cast<uint64_t>(f * m), su); });
        };
        srow.cpu1 = budgeted(budget, [&](double f)
                             { cpu_sobol(f, nullptr); });
        srow.cpuN = budgeted(budget, [&](double f)
                             { cpu_sobol(f, &pool); });
        srow.gpu1.s = seconds([&]
                              { run_gpu(m, {0}); });
        srow.gpu2.s = seconds([&]
                              { run_gpu(m, {0, 1}); });
        sobol.push_back(srow);
        std::fprintf(stderr, "vanilla done\n");
    }

    // ── Scripts ────────────────────────────────────────────────────────────
    struct ScriptRow
    {
        std::string label;
        std::function<std::unique_ptr<qm::ISimulationModel<Real>>()> make;
        std::string source;
    };
    std::vector<ScriptRow> script_rows;
    script_rows.push_back({"Up-and-out, daily, local vol",
                           []
                           {
                               return std::make_unique<qm::LocalVolSimModel<Real>>(100.0, 0.03, 0.01, strikes(),
                                                                                   maturities(), skew(), 1.0 / 252.0);
                           },
                           read_script("up-and-out-call")});
    script_rows.push_back({"Worst-of autocall, 3 assets",
                           []
                           {
                               Eigen::MatrixXd c = Eigen::MatrixXd::Constant(3, 3, 0.5);
                               c.diagonal().setOnes();
                               return std::make_unique<qm::MultiAssetBSSimModel<Real>>(
                                   std::vector<Real>{100, 100, 100}, 0.03, std::vector<Real>{0.01, 0.01, 0.01},
                                   std::vector<Real>{0.25, 0.3, 0.2}, c);
                           },
                           read_script("worst-of-autocall")});
    for (const ScriptRow &sr : script_rows)
    {
        const HostScript h(sr.source, sr.make());
        const uint64_t seed = 7;
        auto gpu = [&](qm::SamplerKind sampler, int paths, int gpus)
        {
            qm::PricingSettings set;
            set.mc_paths = paths;
            set.mc_seed = static_cast<int>(seed);
            set.mc_device = qm::ComputeDevice::Gpu;
            set.mc_sampler = sampler;
            set.mc_gpus = gpus;
            auto m = sr.make();
            return qm::simulate_script(*h.product, *m, set);
        };

        // Pseudo-random, antithetic pairs.
        const qm::SimulationMCResult pilot = gpu(qm::SamplerKind::PseudoRandom, 1 << 18, 2);
        const double unit_var = pilot.std_error() * pilot.std_error() * static_cast<double>(pilot.n_paths / 2);
        const auto n = static_cast<uint64_t>(std::ceil(unit_var / std::pow(target * pilot.npv(), 2)));
        Row row{sr.label};
        row.paths = 2 * static_cast<long long>(n);
        row.cpu1 = budgeted(budget, [&](double f)
                            { cpu_script(h, seed, static_cast<uint64_t>(f * n), nullptr, nullptr); });
        row.cpuN = budgeted(budget, [&](double f)
                            { cpu_script(h, seed, static_cast<uint64_t>(f * n), nullptr, &pool); });
        auto paths_of = [&](double f)
        { return static_cast<int>(std::max(2.0, 2.0 * std::floor(f * n))); };
        row.gpu1 = budgeted(budget, [&](double f)
                            { gpu(qm::SamplerKind::PseudoRandom, paths_of(f), 1); });
        row.gpu2 = budgeted(budget, [&](double f)
                            { gpu(qm::SamplerKind::PseudoRandom, paths_of(f), 2); });
        row.rel_se = target; // n was sized for it (pilot)
        pseudo.push_back(row);

        // Sobol RQMC + bridge: 16 replicates, doubling.
        Row srow{sr.label};
        long long paths = 16LL << 12;
        qm::SimulationMCResult s;
        for (;; paths *= 2)
        {
            s = gpu(qm::SamplerKind::Sobol, static_cast<int>(paths), 2);
            if (s.std_error() / std::abs(s.npv()) <= target || paths >= (16LL << 22))
                break;
        }
        srow.paths = paths;
        srow.rel_se = s.std_error() / std::abs(s.npv());
        const qm::mc::SobolTables tables =
            qm::mc::sobol_tables(h.model->sim_dim(), seed, 16, h.model->brownian_layout(), true);
        const auto m = static_cast<uint64_t>(paths / 16);
        srow.cpu1 = budgeted(budget, [&](double f)
                             { cpu_script(h, seed, static_cast<uint64_t>(f * m), &tables, nullptr); });
        srow.cpuN = budgeted(budget, [&](double f)
                             { cpu_script(h, seed, static_cast<uint64_t>(f * m), &tables, &pool); });
        srow.gpu1 = budgeted(budget, [&](double f)
                             { gpu(qm::SamplerKind::Sobol, static_cast<int>(std::max(16.0, f * paths)), 1); });
        srow.gpu2 = budgeted(budget, [&](double f)
                             { gpu(qm::SamplerKind::Sobol, static_cast<int>(std::max(16.0, f * paths)), 2); });
        sobol.push_back(srow);
        std::fprintf(stderr, "%s done\n", sr.label.c_str());
    }

    // ── Superbucket: the local-vol adjoint (363 risks) ─────────────────────
    {
        auto make = []
        {
            return std::make_unique<qm::LocalVolSimModel<Real>>(100.0, 0.03, 0.01, strikes(), maturities(), skew(),
                                                                1.0 / 252.0);
        };
        const std::string call = "2027-06-01\n    pays max(spot() - 100, 0)\n";
        const HostScript h(call, make());
        const std::size_t np = 3 + strikes().size() * maturities().size();
        const long trail_len = qm::mc::lv_trail_bound(h.product->program(), h.host->view.n_steps);

        auto gpu = [&](qm::SamplerKind sampler, std::size_t paths, int gpus)
        {
            auto m = make();
            std::string why;
            return *qm::simulate_script_aad_gpu(*h.product, *m, paths, 11, why, sampler, gpus);
        };
        // The kernel's adjoint on the host, one trail and gradient row per thread.
        struct Grad
        {
            qm::WelfordAccumulator price;
            std::vector<double> g;
            void add(const std::pair<double, std::vector<double> *> &x)
            {
                price.add(x.first);
                if (g.empty())
                    g.assign(x.second->size(), 0.0);
                for (std::size_t i = 0; i < g.size(); ++i)
                    g[i] += (*x.second)[i];
            }
            void merge(const Grad &o)
            {
                price.merge(o.price);
                if (g.empty())
                    g = o.g;
                else
                    for (std::size_t i = 0; i < g.size() && i < o.g.size(); ++i)
                        g[i] += o.g[i];
            }
        };
        auto cpu = [&](uint64_t units, const qm::mc::SobolTables *tables, qm::ThreadPool *p)
        {
            const qm::mc::ScriptPathView &v = h.host->view;
            const int reps = tables ? tables->replicates : 1;
            auto one = [&](int b, qm::ThreadPool *inner)
            {
                auto fn = [&](uint64_t u)
                {
                    thread_local std::vector<double> trail, grad, pt, out;
                    trail.resize(static_cast<std::size_t>(trail_len));
                    grad.assign(np, 0.0);
                    qm::mc::PathDraws d;
                    d.seed = 11;
                    d.stride = 1;
                    if (tables)
                    {
                        pt.resize(static_cast<std::size_t>(v.n_steps));
                        out.resize(static_cast<std::size_t>(v.n_steps));
                        qm::mc::sobol_bridged_gaussians(tables->view(), static_cast<uint64_t>(b),
                                                        static_cast<uint32_t>(u), pt.data(), 1, out.data(), 1);
                        d.given = out.data();
                    }
                    qm::mc::AdjointScratch w{{trail.data(), 1, 0}, {grad.data(), 1}};
                    const double price = qm::mc::script_lv_adjoint_path(v, d, u, 1.0, w);
                    return std::pair<double, std::vector<double> *>(price, &grad);
                };
                qm::mc::reduce_logical_blocks<Grad>(units, fn, inner);
            };
            if (tables)
                for_replicates(reps, p, [&](int b)
                               { one(b, nullptr); });
            else
                one(0, p);
        };

        const auto pilot = gpu(qm::SamplerKind::PseudoRandom, 1 << 16, 2);
        const double var = std::pow(pilot.risk_std_errors[0], 2) * static_cast<double>(pilot.n_paths);
        const auto n = static_cast<std::size_t>(std::ceil(var / std::pow(target_risk * pilot.risks[0], 2)));
        Row row{"Superbucket (363 risks)"};
        row.paths = static_cast<long long>(n);
        row.cpu1 = budgeted(budget, [&](double f)
                            { cpu(static_cast<uint64_t>(f * n), nullptr, nullptr); });
        row.cpuN = budgeted(budget, [&](double f)
                            { cpu(static_cast<uint64_t>(f * n), nullptr, &pool); });
        auto npaths = [&](double f)
        { return static_cast<std::size_t>(std::max(4096.0, f * n)); };
        row.gpu1 = budgeted(budget, [&](double f)
                            { gpu(qm::SamplerKind::PseudoRandom, npaths(f), 1); });
        row.gpu2 = budgeted(budget, [&](double f)
                            { gpu(qm::SamplerKind::PseudoRandom, npaths(f), 2); });
        row.rel_se = target_risk; // n was sized for it (pilot)
        qm::AADSimulResults g;
        pseudo.push_back(row);

        Row srow{"Superbucket (363 risks)"};
        std::size_t paths = 16u << 10;
        for (;; paths *= 2)
        {
            g = gpu(qm::SamplerKind::Sobol, paths, 2);
            if (g.risk_std_errors[0] / std::abs(g.risks[0]) <= target_risk || paths >= (16u << 20))
                break;
        }
        srow.paths = static_cast<long long>(paths);
        srow.rel_se = g.risk_std_errors[0] / std::abs(g.risks[0]);
        const qm::mc::SobolTables tables =
            qm::mc::sobol_tables(h.model->sim_dim(), 11, 16, h.model->brownian_layout(), true);
        srow.cpu1 = budgeted(budget, [&](double f)
                             { cpu(static_cast<uint64_t>(f * paths / 16), &tables, nullptr); });
        srow.cpuN = budgeted(budget, [&](double f)
                             { cpu(static_cast<uint64_t>(f * paths / 16), &tables, &pool); });
        srow.gpu1 = budgeted(budget, [&](double f)
                             { gpu(qm::SamplerKind::Sobol, static_cast<std::size_t>(std::max(16.0 * 256, f * paths)), 1); });
        srow.gpu2 = budgeted(budget, [&](double f)
                             { gpu(qm::SamplerKind::Sobol, static_cast<std::size_t>(std::max(16.0 * 256, f * paths)), 2); });
        sobol.push_back(srow);
    }
    pool.stop();

    std::printf("Time to a relative standard error of %.0e on the price (%.0e on the delta for the superbucket).\n"
                "CPU columns: the kernels' own per-path code on the host; * = timed on a fraction of the work\n"
                "(budget %.0f s per cell) and scaled linearly.\n",
                target, target_risk, budget);
    print(pseudo, "Pseudo-random (Philox; antithetic pairs except the superbucket)", threads);
    print(sobol, "Sobol RQMC + Brownian bridge (16 replicates)", threads);
    return 0;
}
