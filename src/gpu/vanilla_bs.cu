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

} // namespace quantModeling::gpu
