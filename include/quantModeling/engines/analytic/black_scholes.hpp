#ifndef ENGINE_ANALYTIC_BS_HPP
#define ENGINE_ANALYTIC_BS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
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
        void visit(const EquityFuture &) override;
        void visit(const ZeroCouponBond &) override;
        void visit(const FixedRateBond &) override;

    private:
        static void validate(const VanillaOption &opt);
    };
} // namespace quantModeling

#endif