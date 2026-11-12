#ifndef ENGINE_MC_ASIAN_HPP
#define ENGINE_MC_ASIAN_HPP

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
#include <vector>
#include <utility>

namespace quantModeling
{
    /**
     * Monte Carlo pricing engine for European-style Asian options.
     *
     * This engine supports both arithmetic and geometric average Asian options
     * using Monte Carlo simulation with multiple paths following the Black-Scholes
     * model. Anti-thetic variance reduction is supported.
     */
    class BSEuroAsianMCEngine final : public EngineBase
    {
      public:
        using EngineBase::EngineBase;

        void visit(const AsianOption &) override;
        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine does not support Vanilla options. "
                                        "Use BSEuroVanillaMCEngine instead.");
        }
        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine does not support equity futures.");
        }
        void visit(const ZeroCouponBond &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine does not support bonds.");
        }
        void visit(const FixedRateBond &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine does not support bonds.");
        }
        void visit(const BarrierOption &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine: use BSEuroBarrierMCEngine for barrier options.");
        }
        void visit(const DigitalOption &) override
        {
            throw UnsupportedInstrument("BSEuroAsianMCEngine: use BSDigitalAnalyticEngine for digital options.");
        }

      private:
        static void validate(const AsianOption &opt);
    };

} // namespace quantModeling

#endif
