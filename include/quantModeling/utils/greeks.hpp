#ifndef UTILS_GREEKS_HPP
#define UTILS_GREEKS_HPP

#include "quantModeling/core/types.hpp"

namespace quantModeling
{
    /**
     * Bump sizes of the finite-difference Greeks of the Monte-Carlo engines
     */
    struct GreeksBumps
    {
        Real delta_bump = 0.01;        // 1% spot bumps
        Real vega_bump = 0.001;        // 0.1% vol bumps
        Real rho_bump = 0.0001;        // 0.01% rate bumps
        Real theta_bump = 1.0 / 365.0; // 1 day
    };

} // namespace quantModeling

#endif
