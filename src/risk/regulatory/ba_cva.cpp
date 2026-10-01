#include "quantModeling/risk/regulatory/ba_cva.hpp"

#include <cmath>

namespace quantModeling::ba_cva
{
    namespace
    {
        /// Rate of the supervisory discount factor (MAR50.15, footnote 3).
        constexpr Real discount_rate = 0.05;

        void require_hedge(Real notional, Time maturity)
        {
            if (!(notional >= 0.0) || !(maturity >= 0.0))
                throw InvalidInput("BA-CVA: hedge notional and maturity must be >= 0");
        }

        /// RW_h M_h B_h DF_h of one single-name hedge.
        Real hedge_weight(const SingleNameHedge &hedge)
        {
            require_hedge(hedge.notional, hedge.remaining_maturity);
            return risk_weight(hedge.sector, hedge.quality) * hedge.remaining_maturity *
                   hedge.notional * supervisory_discount_factor(hedge.remaining_maturity);
        }
    } // namespace

    Real risk_weight(Sector sector, CreditQuality quality)
    {
        // Table 1 of MAR50.16: {investment grade, high yield or not rated}.
        const bool ig = quality == CreditQuality::InvestmentGrade;
        switch (sector)
        {
            case Sector::Sovereign:
                return ig ? 0.005 : 0.02;
            case Sector::LocalGovernment:
                return ig ? 0.01 : 0.04;
            case Sector::Financial:
                return ig ? 0.05 : 0.12;
            case Sector::BasicMaterialsEnergyIndustrials:
                return ig ? 0.03 : 0.07;
            case Sector::ConsumerTransportAdministrative:
                return ig ? 0.03 : 0.085;
            case Sector::TechnologyTelecommunications:
                return ig ? 0.02 : 0.055;
            case Sector::HealthCareUtilitiesProfessional:
                return ig ? 0.015 : 0.05;
            case Sector::Other:
                return ig ? 0.05 : 0.12;
        }
        throw InvalidInput("BA-CVA: unknown sector");
    }

    Real supervisory_discount_factor(Time maturity)
    {
        if (!(maturity >= 0.0))
            throw InvalidInput("BA-CVA: maturity must be >= 0");
        const Real x = discount_rate * maturity;
        // -expm1(-x) / x keeps full precision as M -> 0, where DF -> 1.
        return x < 1e-12 ? 1.0 : -std::expm1(-x) / x;
    }

    Real hedge_correlation(HedgeRelation relation)
    {
        // Table 2 of MAR50.26.
        switch (relation)
        {
            case HedgeRelation::Direct:
                return 1.0;
            case HedgeRelation::LegallyRelated:
                return 0.8;
            case HedgeRelation::SameSectorAndRegion:
                return 0.5;
        }
        throw InvalidInput("BA-CVA: unknown hedge relation");
    }

    Real stand_alone_cva(const Counterparty &counterparty)
    {
        Real sum = 0.0;
        for (const NettingSetExposure &ns : counterparty.netting_sets)
        {
            if (!(ns.ead >= 0.0) || !(ns.effective_maturity >= 0.0))
                throw InvalidInput("BA-CVA: EAD and effective maturity must be >= 0");
            const Real df = ns.imm ? 1.0 : supervisory_discount_factor(ns.effective_maturity);
            sum += ns.effective_maturity * ns.ead * df;
        }
        return risk_weight(counterparty.sector, counterparty.quality) * sum / alpha;
    }

    Real k_reduced(const std::vector<Counterparty> &counterparties)
    {
        Real systematic = 0.0;
        Real idiosyncratic = 0.0;
        for (const Counterparty &c : counterparties)
        {
            const Real scva = stand_alone_cva(c);
            systematic += scva;
            idiosyncratic += scva * scva;
        }
        return std::sqrt(rho * rho * systematic * systematic +
                         (1.0 - rho * rho) * idiosyncratic);
    }

    Real k_hedged(const std::vector<Counterparty> &counterparties,
                  const std::vector<IndexHedge> &index_hedges)
    {
        Real ih = 0.0;
        for (const IndexHedge &hedge : index_hedges)
        {
            require_hedge(hedge.notional, hedge.remaining_maturity);
            if (!(hedge.constituent_risk_weight >= 0.0))
                throw InvalidInput("BA-CVA: index risk weight must be >= 0");
            ih += index_diversification * hedge.constituent_risk_weight *
                  hedge.remaining_maturity * hedge.notional *
                  supervisory_discount_factor(hedge.remaining_maturity);
        }

        Real systematic = 0.0;
        Real idiosyncratic = 0.0;
        Real misalignment = 0.0;
        for (const Counterparty &c : counterparties)
        {
            Real snh = 0.0;
            for (const SingleNameHedge &hedge : c.hedges)
            {
                const Real r = hedge_correlation(hedge.relation);
                const Real weight = hedge_weight(hedge);
                snh += r * weight;
                misalignment += (1.0 - r * r) * weight * weight;
            }
            const Real net = stand_alone_cva(c) - snh;
            systematic += net;
            idiosyncratic += net * net;
        }
        const Real first = rho * systematic - ih;
        return std::sqrt(first * first + (1.0 - rho * rho) * idiosyncratic + misalignment);
    }

    Real capital_reduced(const std::vector<Counterparty> &counterparties)
    {
        return discount_scalar * k_reduced(counterparties);
    }

    Real capital_full(const std::vector<Counterparty> &counterparties,
                      const std::vector<IndexHedge> &index_hedges)
    {
        return discount_scalar * (beta * k_reduced(counterparties) +
                                  (1.0 - beta) * k_hedged(counterparties, index_hedges));
    }

} // namespace quantModeling::ba_cva
