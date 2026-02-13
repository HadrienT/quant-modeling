#ifndef ENGINE_MC_ASIAN_HPP
#define ENGINE_MC_ASIAN_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
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

    private:
        static void validate(const AsianOption &opt);

        /**
         * Simulate a single path and compute the payoff for an arithmetic average Asian option
         * @param S0 Initial spot price
         * @param T Maturity time
         * @param r Risk-free rate
         * @param q Dividend yield
         * @param sigma Volatility
         * @param num_dates Number of monitoring dates
         * @param payoff The payoff function to apply to the average
         * @param rng Random number generator
         * @param gaussian_gen Gaussian random variable generator (may be antithetic-aware)
         * @return Payoff value for this path
         */
        static Real simulate_arithmetic_path(
            Real S0, Real T, Real r, Real q, Real sigma,
            int num_dates,
            const IPayoff &payoff,
            Pcg32 &rng,
            AntitheticGaussianGenerator &gaussian_gen);

        /**
         * Simulate a single path and compute the payoff for a geometric average Asian option
         * @param S0 Initial spot price
         * @param T Maturity time
         * @param r Risk-free rate
         * @param q Dividend yield
         * @param sigma Volatility
         * @param num_dates Number of monitoring dates
         * @param payoff The payoff function to apply to the geometric mean
         * @param rng Random number generator
         * @param gaussian_gen Gaussian random variable generator (may be antithetic-aware)
         * @return Payoff value for this path
         */
        static Real simulate_geometric_path(
            Real S0, Real T, Real r, Real q, Real sigma,
            int num_dates,
            const IPayoff &payoff,
            Pcg32 &rng,
            AntitheticGaussianGenerator &gaussian_gen);

        /**
         * Simulate arithmetic path and return {payoff, average} for pathwise Greeks
         * @return pair of {payoff_value, arithmetic_average}
         */
        static std::pair<Real, Real> simulate_arithmetic_path_with_average(
            Real S0, Real T, Real r, Real q, Real sigma,
            int num_dates,
            const IPayoff &payoff,
            Pcg32 &rng,
            AntitheticGaussianGenerator &gaussian_gen);

        /**
         * Simulate geometric path and return {payoff, geometric_mean} for pathwise Greeks
         * @return pair of {payoff_value, geometric_mean}
         */
        static std::pair<Real, Real> simulate_geometric_path_with_average(
            Real S0, Real T, Real r, Real q, Real sigma,
            int num_dates,
            const IPayoff &payoff,
            Pcg32 &rng,
            AntitheticGaussianGenerator &gaussian_gen);
    };

} // namespace quantModeling

#endif
