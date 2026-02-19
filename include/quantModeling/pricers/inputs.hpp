#ifndef PRICERS_INPUTS_HPP
#define PRICERS_INPUTS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include <vector>

namespace quantModeling
{

    struct VanillaBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
        int tree_steps = 100;
        int pde_space_steps = 100;
        int pde_time_steps = 100;
    };

    struct AmericanVanillaBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;

        int tree_steps = 100;      // For binomial/trinomial trees
        int pde_space_steps = 100; // For PDE
        int pde_time_steps = 100;  // For PDE
    };

    struct AsianBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        AsianAverageType average_type = AsianAverageType::Arithmetic;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
    };

    struct EquityFutureInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real notional = 1.0;
    };

    struct ZeroCouponBondInput
    {
        Time maturity;
        Real rate;
        Real notional = 1.0;
        std::vector<Time> discount_times;
        std::vector<Real> discount_factors;
    };

    struct FixedRateBondInput
    {
        Time maturity;
        Real rate;
        Real coupon_rate;
        int coupon_frequency = 1;
        Real notional = 1.0;
        std::vector<Time> discount_times;
        std::vector<Real> discount_factors;
    };

} // namespace quantModeling

#endif
