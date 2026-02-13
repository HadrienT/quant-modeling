#ifndef ENGINE_ANALYTIC_BS_HPP
#define ENGINE_ANALYTIC_BS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>

namespace quantModeling
{
    class BSEuroVanillaAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const VanillaOption &opt) override;
        void visit(const AsianOption &) override;

    private:
        static void validate(const VanillaOption &opt);
    };
} // namespace quantModeling

#endif