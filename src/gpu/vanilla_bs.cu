#include "quantModeling/gpu/vanilla_bs.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        template <OptionType CP, bool Antithetic, bool IS>
        mc::VanillaStats run(const VanillaGpuRequest &req)
        {
            const mc::VanillaPhiloxUnit<CP, Antithetic, IS> unit{req.spec, req.seed, req.is_shift};
            return detail::run_logical_blocks<mc::VanillaStats>(req.device, req.n_units, unit,
                                                                req.max_blocks_per_launch);
        }

        template <OptionType CP>
        mc::VanillaStats dispatch_flags(const VanillaGpuRequest &req)
        {
            const bool is = (req.is_shift != Real(0));
            if (req.antithetic)
                return is ? run<CP, true, true>(req) : run<CP, true, false>(req);
            return is ? run<CP, false, true>(req) : run<CP, false, false>(req);
        }
    } // namespace

    mc::VanillaStats simulate_vanilla_terminal(const VanillaGpuRequest &req)
    {
        return req.type == OptionType::Call ? dispatch_flags<OptionType::Call>(req)
                                            : dispatch_flags<OptionType::Put>(req);
    }

    namespace
    {
        template <OptionType CP, bool IS>
        mc::VanillaStats run_sobol(const VanillaSobolGpuRequest &req)
        {
            mc::VanillaSobolUnit<CP, IS> unit;
            unit.spec = req.spec;
            for (int k = 0; k < 32; ++k)
                unit.V[k] = req.directions[k];
            unit.shift = req.shift;
            unit.is_shift = req.is_shift;
            return detail::run_logical_blocks<mc::VanillaStats>(req.device, req.n_points, unit,
                                                                req.max_blocks_per_launch);
        }
    } // namespace

    mc::VanillaStats simulate_vanilla_sobol(const VanillaSobolGpuRequest &req)
    {
        if (req.n_points > (uint64_t{1} << 32))
            throw InvalidInput("simulate_vanilla_sobol: at most 2^32 points per replicate");
        const bool is = (req.is_shift != Real(0));
        if (req.type == OptionType::Call)
            return is ? run_sobol<OptionType::Call, true>(req) : run_sobol<OptionType::Call, false>(req);
        return is ? run_sobol<OptionType::Put, true>(req) : run_sobol<OptionType::Put, false>(req);
    }

    void warm_up(int device)
    {
        VanillaGpuRequest req;
        req.spec.S0 = req.spec.K = req.spec.movedSpot = req.spec.movedSpot_upT = req.spec.movedSpot_dnT = 100.0;
        req.spec.sigma = req.spec.T = req.spec.sqrtT = req.spec.df = req.spec.df_upT = req.spec.df_dnT = 1.0;
        req.spec.rootVariance = req.spec.rootVariance_upT = req.spec.rootVariance_dnT = 0.2;
        req.spec.dS = req.spec.theta_bump = 1.0;
        req.spec.factor_up = req.spec.factor_dn = 1.0;
        req.n_units = 1;
        req.device = device;
        for (OptionType type : {OptionType::Call, OptionType::Put})
            for (bool anti : {false, true})
                for (Real is : {0.0, 0.1})
                {
                    req.type = type;
                    req.antithetic = anti;
                    req.is_shift = is;
                    simulate_vanilla_terminal(req);
                    VanillaSobolGpuRequest sob;
                    sob.spec = req.spec;
                    sob.type = type;
                    sob.is_shift = is;
                    sob.n_points = 1;
                    sob.device = device;
                    simulate_vanilla_sobol(sob);
                }
    }

} // namespace quantModeling::gpu
