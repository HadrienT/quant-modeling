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
    struct SampleDef
    {
        bool numeraire = true;             ///< need the numeraire at this date
        std::vector<Time> discount_mats;   ///< P(t, T) for these maturities T
        std::vector<Time> forward_mats;    ///< F(t, T) for these maturities T
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

        void allocate(const SampleDef &def, std::size_t n_underlyings)
        {
            spots.resize(n_underlyings);
            discounts.resize(def.discount_mats.size());
            forwards.resize(def.forward_mats.size());
        }

        void initialize()
        {
            numeraire = T(1);
            std::fill(spots.begin(), spots.end(), T(0));
            std::fill(discounts.begin(), discounts.end(), T(1));
            std::fill(forwards.begin(), forwards.end(), T(0));
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
