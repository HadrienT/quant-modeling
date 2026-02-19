#ifndef ENGINE_TREE_BINOMIAL_HPP
#define ENGINE_TREE_BINOMIAL_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <cmath>
#include <vector>

namespace quantModeling
{
    /**
     * @brief Binomial tree engine for vanilla options
     *
     * Implements Cox-Ross-Rubinstein (CRR) binomial tree model with:
     * - N time steps (configurable, default 100)
     * - Works with any ILocalVolModel (Black-Scholes, local vol surfaces, etc.)
     * - Supports both American and European exercise styles
     * - Backward induction from maturity with early exercise check
     */
    class BinomialVanillaEngine final : public EngineBase
    {
    public:
        explicit BinomialVanillaEngine(PricingContext ctx);

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
