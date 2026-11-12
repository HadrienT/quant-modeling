#include "quantModeling/models/rates/short_rate_model.hpp"

#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    void check_bond_option_terms(const char *who, Real K, Time T_option, Time T_bond)
    {
        const std::string name(who);
        if (T_option <= 0.0)
            throw InvalidInput(name + "::bond_option_price: T_option must be > 0");
        if (T_bond <= T_option)
            throw InvalidInput(name + "::bond_option_price: T_bond must be > T_option");
        if (K <= 0.0)
            throw InvalidInput(name + "::bond_option_price: strike K must be > 0");
    }

    Real gaussian_bond_option_price(bool is_call, Real K, Time T_option, Time T_bond, Real P0_option,
                                    Real P0_bond, Real a, Real sigma)
    {
        const Real P0S = P0_bond;
        const Real P0T = P0_option;

        const Real sigma_p = (sigma / a) * (1.0 - std::exp(-a * (T_bond - T_option))) *
                             std::sqrt((1.0 - std::exp(-2.0 * a * T_option)) / (2.0 * a));

        const Real h = (1.0 / sigma_p) * std::log(P0S / (K * P0T)) + 0.5 * sigma_p;

        if (is_call)
            return P0S * norm_cdf(h) - K * P0T * norm_cdf(h - sigma_p);
        else
            return K * P0T * norm_cdf(-h + sigma_p) - P0S * norm_cdf(-h);
    }

} // namespace quantModeling
