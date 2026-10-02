#include "quantModeling/risk/regulatory/sa_cva.hpp"

#include <cmath>

namespace quantModeling::sa_cva
{

    Real credit_spread_risk_weight(ba_cva::Sector sector, ba_cva::CreditQuality quality)
    {
        const bool ig = quality == ba_cva::CreditQuality::InvestmentGrade;
        // Table 7 of MAR50.65, buckets 1 a), 1 b), 2, ..., 7.
        switch (sector)
        {
            case ba_cva::Sector::Sovereign:
                return ig ? 0.005 : 0.02;
            case ba_cva::Sector::LocalGovernment:
                return ig ? 0.01 : 0.04;
            case ba_cva::Sector::Financial:
                return ig ? 0.05 : 0.12;
            case ba_cva::Sector::BasicMaterialsEnergyIndustrials:
                return ig ? 0.03 : 0.07;
            case ba_cva::Sector::ConsumerTransportAdministrative:
                return ig ? 0.03 : 0.085;
            case ba_cva::Sector::TechnologyTelecommunications:
                return ig ? 0.02 : 0.055;
            case ba_cva::Sector::HealthCareUtilitiesProfessional:
                return ig ? 0.015 : 0.05;
            case ba_cva::Sector::Other:
                return ig ? 0.05 : 0.12;
        }
        throw InvalidInput("SA-CVA: unknown sector");
    }

    Capital capital(const Sensitivities &s, Real multiplier)
    {
        if (!(multiplier >= 1.0))
            throw InvalidInput("SA-CVA: the multiplier m_CVA must be >= 1");
        const auto finite = [](Real x)
        {
            if (!std::isfinite(x))
                throw InvalidInput("SA-CVA: a sensitivity is not finite");
            return x;
        };
        Capital out;

        // Interest-rate delta: one bucket, the currency (MAR50.54, 50.56).
        std::array<Real, 5> ws{};
        Real sum = 0.0;
        for (std::size_t k = 0; k < 5; ++k)
            ws[k] = interest_rate_risk_weights[k] * finite(s.interest_rate_delta[k]);
        for (std::size_t k = 0; k < 5; ++k)
            for (std::size_t l = 0; l < 5; ++l)
                sum += interest_rate_correlations[k][l] * ws[k] * ws[l];
        out.interest_rate_delta = multiplier * std::sqrt(std::max(sum, 0.0));

        // Interest-rate vega: one risk factor (MAR50.58).
        out.interest_rate_vega =
            multiplier * std::abs(interest_rate_vega_risk_weight * finite(s.interest_rate_vega));

        // Counterparty credit spread delta: one name, five tenors (MAR50.65).
        const Real rw = credit_spread_risk_weight(s.sector, s.quality);
        sum = 0.0;
        for (std::size_t k = 0; k < 5; ++k)
            ws[k] = rw * finite(s.credit_spread_delta[k]);
        for (std::size_t k = 0; k < 5; ++k)
            for (std::size_t l = 0; l < 5; ++l)
                sum += (k == l ? 1.0 : credit_spread_tenor_correlation) * ws[k] * ws[l];
        out.credit_spread_delta = multiplier * std::sqrt(std::max(sum, 0.0));
        return out;
    }

} // namespace quantModeling::sa_cva
