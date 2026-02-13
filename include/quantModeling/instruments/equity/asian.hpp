#ifndef INSTRUMENT_EQUITY_ASIAN_HPP
#define INSTRUMENT_EQUITY_ASIAN_HPP

#include "quantModeling/instruments/base.hpp"
#include <memory>
#include <vector>
#include <cmath>

namespace quantModeling
{
    enum class AsianAverageType
    {
        Arithmetic,
        Geometric
    };

    struct AsianOption final : Instrument
    {
        std::shared_ptr<const IPayoff> payoff;
        std::shared_ptr<const IExercise> exercise;
        AsianAverageType average_type = AsianAverageType::Arithmetic;
        Real notional = 1.0;

        AsianOption(std::shared_ptr<const IPayoff> p, std::shared_ptr<const IExercise> e,
                    AsianAverageType avg_type = AsianAverageType::Arithmetic, Real n = 1.0)
            : payoff(std::move(p)), exercise(std::move(e)), average_type(avg_type), notional(n)
        {
        }
        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    // ========== Asian Payoff Implementations ==========

    /**
     * Arithmetic Average Asian Payoff
     * Payoff is max(Average(S) - K, 0) for call or max(K - Average(S), 0) for put
     * Where Average is computed from observed spot prices
     */
    struct ArithmeticAsianPayoff final : IPayoff
    {
        OptionType t_;
        Real K_;

        ArithmeticAsianPayoff(OptionType t, Real K) : t_(t), K_(K) {}

        OptionType type() const override { return t_; }
        Real strike() const override { return K_; }

        /**
         * Compute payoff from the arithmetic average spot price
         * @param average_spot The arithmetic average of spot prices
         */
        Real operator()(Real average_spot) const override
        {
            if (t_ == OptionType::Call)
                return std::max(average_spot - K_, 0.0);
            return std::max(K_ - average_spot, 0.0);
        }
    };

    /**
     * Geometric Average Asian Payoff
     * Payoff is max(Geometric_Average(S) - K, 0) for call or max(K - Geometric_Average(S), 0) for put
     * Where Geometric_Average is computed as (product of spot prices)^(1/n)
     */
    struct GeometricAsianPayoff final : IPayoff
    {
        OptionType t_;
        Real K_;

        GeometricAsianPayoff(OptionType t, Real K) : t_(t), K_(K) {}

        OptionType type() const override { return t_; }
        Real strike() const override { return K_; }

        /**
         * Compute payoff from the geometric average spot price
         * @param geometric_average The geometric average of spot prices
         */
        Real operator()(Real geometric_average) const override
        {
            if (t_ == OptionType::Call)
                return std::max(geometric_average - K_, 0.0);
            return std::max(K_ - geometric_average, 0.0);
        }
    };

} // namespace quantModeling

#endif