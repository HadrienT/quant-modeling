#include "quantModeling/risk/regulatory/irb.hpp"

#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>

namespace quantModeling::irb
{
    namespace
    {
        void require_pd(Real pd)
        {
            if (!(pd > 0.0 && pd < 1.0))
                throw InvalidInput("IRB: probability of default must be in (0, 1)");
        }
    } // namespace

    Real asset_correlation(Real pd, bool large_financial)
    {
        require_pd(pd);
        const Real weight = (1.0 - std::exp(-50.0 * pd)) / (1.0 - std::exp(-50.0));
        const Real correlation = 0.12 * weight + 0.24 * (1.0 - weight);
        return large_financial ? 1.25 * correlation : correlation;
    }

    Real maturity_adjustment(Real pd)
    {
        require_pd(pd);
        const Real b = 0.11852 - 0.05478 * std::log(pd);
        return b * b;
    }

    Real capital_requirement(Real pd, Real lgd, Time maturity, bool large_financial)
    {
        require_pd(pd);
        if (!(lgd >= 0.0 && lgd <= 1.0))
            throw InvalidInput("IRB: loss given default must be in [0, 1]");
        if (!(maturity > 0.0))
            throw InvalidInput("IRB: effective maturity must be > 0");
        const Real r = asset_correlation(pd, large_financial);
        const Real b = maturity_adjustment(pd);
        // Default probability conditional on the 99.9 % systematic scenario.
        const Real stressed_pd = norm_cdf(
            (inverse_normal_cdf(pd) + std::sqrt(r) * inverse_normal_cdf(confidence)) /
            std::sqrt(1.0 - r));
        return lgd * (stressed_pd - pd) * (1.0 + (maturity - 2.5) * b) / (1.0 - 1.5 * b);
    }

    Real risk_weighted_assets(Real capital_requirement, Real ead)
    {
        if (!(capital_requirement >= 0.0) || !(ead >= 0.0))
            throw InvalidInput("IRB: capital requirement and EAD must be >= 0");
        return rwa_multiplier * capital_requirement * ead;
    }

} // namespace quantModeling::irb
