#ifndef ENGINE_TREE_TRINOMIAL_HPP
#define ENGINE_TREE_TRINOMIAL_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <cmath>
#include <vector>

namespace quantModeling
{
    /**
     * @brief Trinomial tree engine for vanilla options
     *
     * Implements Boyle's trinomial tree model with:
     * - N time steps (configurable, default 100)
     * - Works with any ILocalVolModel (Black-Scholes, local vol surfaces, etc.)
     * - Supports both American and European exercise styles
     * - Three branches per node for better convergence
     */
    class TrinomialVanillaEngine final : public EngineBase
    {
    public:
        explicit TrinomialVanillaEngine(PricingContext ctx);

        void visit(const VanillaOption &opt) override;
        void visit(const AsianOption &) override;
        void visit(const EquityFuture &) override;
        void visit(const ZeroCouponBond &) override;
        void visit(const FixedRateBond &) override;

    private:
        int steps_;
        static void validate(const VanillaOption &opt);
    };
} // namespace quantModeling

#endif
