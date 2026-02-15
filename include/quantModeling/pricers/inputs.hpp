#ifndef PRICERS_INPUTS_HPP
#define PRICERS_INPUTS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/equity/asian.hpp"

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

} // namespace quantModeling

#endif
