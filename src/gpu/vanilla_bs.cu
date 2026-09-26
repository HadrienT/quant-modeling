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
                }
    }

} // namespace quantModeling::gpu
