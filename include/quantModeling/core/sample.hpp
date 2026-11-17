#ifndef QM_CORE_SAMPLE_HPP
#define QM_CORE_SAMPLE_HPP

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @brief What a product needs sampled at one event date.
     *
     * The product fills a vector<SampleDef>, one entry per entry of its
     * timeline(). The model reads it in allocate()/init() to size its output
     * and to know which discount factors / forwards to evaluate.
     */
    /// What the model computes for a slot of Sample::monitored.
    ///
    ///  - `Survival` (survivalAbove / survivalBelow, WP 26 P2): the
    ///    probability that the spot of `asset` stayed above (`above`) or
    ///    below `level` since the previous event date, conditional on the
    ///    simulated path -- the product of the Brownian-bridge survival
    ///    probabilities over the model's own steps.
    ///  - `LastStepCall` / `LastStepPut` / `LastStepDigital` (lastStepCall /
    ///    lastStepPut / lastStepDigital, WP 27 §8 rung 1): the expectation of
    ///    max(S - K, 0), max(K - S, 0) or 1{S > K} at this date, K = `level`,
    ///    over the model's last step into it, given the path before that step
    ///    -- Black's formula on the step's lognormal law. Same expectation as
    ///    the payoff itself, but smooth to every order in the parameters
    ///    along a path, so that pathwise second-order risks are exact.
    enum class StepQuantity : unsigned char
    {
        Survival,
        LastStepCall,
        LastStepPut,
        LastStepDigital,
    };

    /// One slot of Sample::monitored: a quantity the model computes over its
    /// steps since the previous event (see StepQuantity).
    struct StepMonitor
    {
        Real level = 0.0;
        std::size_t asset = 0;
        bool above = true; ///< Survival only
        StepQuantity kind = StepQuantity::Survival;

        bool operator==(const StepMonitor &) const = default;
    };

    struct SampleDef
    {
        bool numeraire = true;             ///< need the numeraire at this date
        std::vector<Time> discount_mats;   ///< P(t, T) for these maturities T
        std::vector<Time> forward_mats;    ///< F(t, T) for these maturities T
        std::vector<StepMonitor> monitors; ///< survivals and last-step expectations (StepQuantity)
        /// Ask for Sample::inst_vol: the instantaneous volatility of asset 0
        /// at this date, on the path's state (engines/mc/path_distribution.hpp).
        /// Off for pricing; a model that does not fill it leaves it untouched.
        bool inst_vol = false;
    };

    /**
     * @brief The market state at one event date on one simulated path.
     *
     * Templated on the number type so the same struct serves double pricing
     * and (later) an AAD tape type without touching any signature.
     */
    template <class T = Real>
    struct Sample
    {
        T numeraire{};
        std::vector<T> spots;     ///< underlying level(s), one per asset
        std::vector<T> discounts; ///< aligned with SampleDef::discount_mats
        std::vector<T> forwards;  ///< aligned with SampleDef::forward_mats
        std::vector<T> monitored; ///< aligned with SampleDef::monitors
        /// sigma(t) of asset 0 on this path, when SampleDef::inst_vol: the
        /// diffusion coefficient of d ln S at the path's state at t
        /// (sqrt(v_t) for Heston, sigma_loc(S_t, t) for local vol...).
        T inst_vol{};

        void allocate(const SampleDef &def, std::size_t n_underlyings)
        {
            spots.resize(n_underlyings);
            discounts.resize(def.discount_mats.size());
            forwards.resize(def.forward_mats.size());
            monitored.resize(def.monitors.size());
        }

        void initialize()
        {
            numeraire = T(1);
            std::fill(spots.begin(), spots.end(), T(0));
            std::fill(discounts.begin(), discounts.end(), T(1));
            std::fill(forwards.begin(), forwards.end(), T(0));
            std::fill(monitored.begin(), monitored.end(), T(1));
        }
    };

    /// One simulated path: the market state at each of the product's event
    /// dates, in timeline order.
    template <class T = Real>
    using Scenario = std::vector<Sample<T>>;

    /// Size every Sample in a scenario from a defline.
    template <class T>
    inline void allocate_scenario(Scenario<T> &scen,
                                  const std::vector<SampleDef> &defline,
                                  std::size_t n_underlyings)
    {
        scen.resize(defline.size());
        for (std::size_t i = 0; i < defline.size(); ++i)
            scen[i].allocate(defline[i], n_underlyings);
    }

} // namespace quantModeling

#endif // QM_CORE_SAMPLE_HPP
