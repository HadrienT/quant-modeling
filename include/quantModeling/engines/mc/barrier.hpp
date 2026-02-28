#ifndef ENGINE_MC_BARRIER_HPP
#define ENGINE_MC_BARRIER_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/rng.hpp"

namespace quantModeling
{

    /**
     * Monte Carlo pricing engine for European barrier options.
     *
     * Supports all four barrier flavours:
     *   UpAndIn / UpAndOut / DownAndIn / DownAndOut
     * combined with call or put vanilla payoffs.
     *
     * Barrier monitoring:
     *   - Daily discrete steps (252 per year) by default, or user-specified.
     *   - Optional Brownian-bridge correction (Andersen & Brotherton-Ratcliffe)
     *     to approximate continuous monitoring.
     *
     * Greeks are computed via central finite differences with common random
     * numbers (CRN):  all bumped paths use the same Gaussian and BB-uniform
     * draws as the base path, minimising variance on the FD estimates.
     */
    class BSEuroBarrierMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const BarrierOption &opt) override;

        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine: use BSEuroVanillaMCEngine for vanilla.");
        }

        void visit(const AsianOption &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine: use BSEuroAsianMCEngine for Asian.");
        }

        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine does not support equity futures.");
        }

        void visit(const ZeroCouponBond &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine does not support bonds.");
        }

        void visit(const FixedRateBond &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine does not support bonds.");
        }

        void visit(const DigitalOption &) override
        {
            throw UnsupportedInstrument("BSEuroBarrierMCEngine: use BSDigitalAnalyticEngine for digital options.");
        }

    private:
        static void validate(const BarrierOption &opt);
    };

} // namespace quantModeling

#endif // ENGINE_MC_BARRIER_HPP
