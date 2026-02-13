#ifndef ENGINE_ANALYTIC_ASIAN_HPP
#define ENGINE_ANALYTIC_ASIAN_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>

namespace quantModeling
{
    /**
     * Analytic pricing engine for European-style arithmetic Asian options
     * using the Turnbull-Wakeman approximation.
     *
     * This engine assumes a Black-Scholes model and prices arithmetic average
     * Asian options using moment matching techniques.
     */
    class BSEuroArithmeticAsianAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const AsianOption &opt) override;
        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSEuroArithmeticAsianAnalyticEngine does not support Vanilla options");
        }

    private:
        static void validate(const AsianOption &opt);
    };

    /**
     * Analytic pricing engine for European-style geometric Asian options.
     *
     * Geometric Asian options have a closed-form solution under Black-Scholes
     * assumptions. The effective volatility and drift are adjusted to match
     * the properties of the geometric average.
     */
    class BSEuroGeometricAsianAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const AsianOption &opt) override;
        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSEuroGeometricAsianAnalyticEngine does not support Vanilla options");
        }

    private:
        static void validate(const AsianOption &opt);
    };

} // namespace quantModeling

#endif
