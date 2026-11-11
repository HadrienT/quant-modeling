#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/engines/mc/spot_controls.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/instruments/scripted_product.hpp"

#include "library_scripts.hpp"

// Lot P1g of blueprint/wp/26-performance.md: the units of a logical block run
// one per thread and their samples are reduced afterwards, in the order the
// serial kernel adds them. The partial statistics must be the serial ones bit
// for bit -- the GPU-vs-CPU tests compare to 1e-9 and could not see a change
// of summation order. Every library script, every GPU model, every
// estimator the parallel path serves: plain (Philox, antithetic), Sobol,
// stratified, spot controls, forward-mode duals.

namespace quantModeling
{
    namespace
    {
        using namespace gpu_test;

        template <class T>
        bool same_bits(const std::vector<T> &a, const std::vector<T> &b)
        {
            return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
        }

        bool same_bits(const gpu::ScriptGpuStats &a, const gpu::ScriptGpuStats &b)
        {
            return same_bits(a.plain, b.plain) && same_bits(a.controlled, b.controlled) &&
                   same_bits(a.stratified_controlled, b.stratified_controlled);
        }

        bool same_bits(const std::vector<std::vector<WelfordAccumulator>> &a,
                       const std::vector<std::vector<WelfordAccumulator>> &b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!same_bits(a[i], b[i]))
                    return false;
            return true;
        }

        enum class Estimator
        {
            Plain,
            Sobol,
            Stratified,
            Controls,
            Duals
        };

        const char *name(Estimator e)
        {
            switch (e)
            {
                case Estimator::Plain:
                    return "plain";
                case Estimator::Sobol:
                    return "Sobol";
                case Estimator::Stratified:
                    return "stratified";
                case Estimator::Controls:
                    return "spot controls";
                default:
                    return "duals";
            }
        }

        /// Serial and parallel units on every library script under `dyn`.
        void expect_parallel_equals_serial(Dyn dyn, bool single_asset_only, Estimator est)
        {
            int compared = 0;
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                if (single_asset_only && product.n_underlyings() > 1)
                    continue;
                auto m = model(dyn, product.n_underlyings());
                m->init(product.timeline(), product.defline());
                DeviceModel dm;
                if (!m->describe_device(dm))
                    continue;
                if (est == Estimator::Duals)
                {
                    const int dirs = mc::dual_directions(dm.kind, dm.n_assets);
                    if (dirs <= 0 || dirs > 8)
                        continue;
                }

                gpu::ScriptGpuRequest req;
                req.program = &product.program();
                req.model = &dm;
                req.baseline = product.baseline();
                for (const SampleDef &def : product.defline())
                    req.discount_mats.push_back(def.discount_mats);
                if (!gpu::script_gpu_unsupported(product.program(), dm, req.discount_mats).empty())
                    continue;
                // 3 logical blocks and a partial one: full blocks, the tail,
                // and replicates where the estimator has them.
                req.n_units = 3 * 4096 + 1000;
                req.seed = 23;
                req.antithetic = est == Estimator::Plain || est == Estimator::Controls;
                req.compile = false;
                std::optional<mc::SobolTables> tables;
                mc::SpotControlSet controls;
                if (est == Estimator::Sobol)
                {
                    req.n_units = 4096 + 500;
                    tables = mc::sobol_tables(m->sim_dim(), req.seed, 4, m->brownian_layout(), true, false);
                    req.sobol = &*tables;
                }
                if (est == Estimator::Stratified)
                {
                    req.stratified = true;
                    req.replicates = 3;
                }
                if (est == Estimator::Controls)
                {
                    controls = mc::choose_spot_controls(product.timeline(), *m);
                    req.controls = controls.sel;
                }

                const std::string what = s.name + " (" + name(est) + ")";
                gpu::ScriptGpuRequest serial = req;
                serial.parallel_units = false;
                if (est == Estimator::Duals)
                {
                    const auto a = gpu::simulate_script_duals(serial);
                    const auto b = gpu::simulate_script_duals(req);
                    EXPECT_TRUE(same_bits(a, b)) << what;
                }
                else
                {
                    const gpu::ScriptGpuStats a = gpu::simulate_script(serial);
                    const gpu::ScriptGpuStats b = gpu::simulate_script(req);
                    EXPECT_TRUE(same_bits(a, b)) << what;
                }
                ++compared;
            }
            EXPECT_GT(compared, 0);
        }
    } // namespace

    class GpuParallelUnitsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuParallelUnitsTest, BlackScholesPlain)
    {
        expect_parallel_equals_serial(Dyn::BlackScholes, false, Estimator::Plain);
    }
    TEST_F(GpuParallelUnitsTest, BlackScholesSobol)
    {
        expect_parallel_equals_serial(Dyn::BlackScholes, false, Estimator::Sobol);
    }
    TEST_F(GpuParallelUnitsTest, BlackScholesStratified)
    {
        expect_parallel_equals_serial(Dyn::BlackScholes, true, Estimator::Stratified);
    }
    TEST_F(GpuParallelUnitsTest, BlackScholesControls)
    {
        expect_parallel_equals_serial(Dyn::BlackScholes, false, Estimator::Controls);
    }
    TEST_F(GpuParallelUnitsTest, BlackScholesDuals)
    {
        expect_parallel_equals_serial(Dyn::BlackScholes, false, Estimator::Duals);
    }
    TEST_F(GpuParallelUnitsTest, LocalVolPlainAndSobol)
    {
        expect_parallel_equals_serial(Dyn::LocalVol, true, Estimator::Plain);
        expect_parallel_equals_serial(Dyn::LocalVol, true, Estimator::Sobol);
    }
    TEST_F(GpuParallelUnitsTest, HestonPlainAndDuals)
    {
        expect_parallel_equals_serial(Dyn::Heston, true, Estimator::Plain);
        expect_parallel_equals_serial(Dyn::Heston, true, Estimator::Duals);
    }
    TEST_F(GpuParallelUnitsTest, SlvPlain)
    {
        expect_parallel_equals_serial(Dyn::SLV, true, Estimator::Plain);
    }

} // namespace quantModeling
