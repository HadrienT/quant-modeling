#ifndef ENGINE_PDE_EUROPEAN_VANILLA_HPP
#define ENGINE_PDE_EUROPEAN_VANILLA_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <cmath>
#include <vector>

namespace quantModeling
{
    /**
     * @brief PDE finite difference engine for European vanilla options
     *
     * Implements Crank-Nicolson finite difference scheme for the Black-Scholes PDE:
     * dV/dt + 0.5 * sigma^2 * S^2 * d2V/dS2 + (r-q) * S * dV/dS - r * V = 0
     *
     * Features:
     * - Crank-Nicolson time discretization (semi-implicit, unconditionally stable)
     * - Central differences in space
     * - Thomas algorithm for efficient tridiagonal solve
     * - Works with any ILocalVolModel
     * - European exercise only (no early exercise check)
     */
    class PDEEuropeanVanillaEngine final : public EngineBase
    {
    public:
        explicit PDEEuropeanVanillaEngine(PricingContext ctx);

        void visit(const VanillaOption &opt) override;
        void visit(const AsianOption &) override;
        void visit(const EquityFuture &) override;
        void visit(const ZeroCouponBond &) override;
        void visit(const FixedRateBond &) override;

    private:
        int M_; // space steps
        int N_; // time steps

        static void validate(const VanillaOption &opt);

        // Thomas algorithm for tridiagonal system Ax = b
        static void solve_tridiagonal(const std::vector<Real> &a,
                                      const std::vector<Real> &b,
                                      const std::vector<Real> &c,
                                      const std::vector<Real> &d,
                                      std::vector<Real> &x);
    };
} // namespace quantModeling

#endif
