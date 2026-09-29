#include "quantModeling/market/credit_bootstrap.hpp"

#include "quantModeling/engines/analytic/cds.hpp"

#include <algorithm>
#include <string>

namespace quantModeling
{
    namespace
    {
        /// Buyer's value per unit notional of the par CDS for `quote`, on the
        /// curve made of the fixed `times`/`hazards` plus `trial` on the new
        /// segment: increasing in `trial`.
        Real buyer_value(const std::vector<Time> &times, const std::vector<Real> &hazards,
                         const CreditDefaultSwap &cds, const DiscountCurve &discount,
                         Real recovery, Real trial)
        {
            std::vector<Time> t = times;
            std::vector<Real> h = hazards;
            t.push_back(cds.maturity());
            h.push_back(trial);
            const CdsLegs legs = cds_legs(cds, discount, CreditCurve(std::move(t), std::move(h)), recovery);
            return legs.protection - cds.spread * legs.risky_annuity;
        }
    } // namespace

    CreditCurve bootstrap_credit_curve(std::vector<CdsQuote> quotes, const DiscountCurve &discount,
                                       Real recovery, int frequency)
    {
        if (quotes.empty())
            throw InvalidInput("bootstrap_credit_curve: need at least one quote");
        if (!(recovery >= 0.0 && recovery < 1.0))
            throw InvalidInput("bootstrap_credit_curve: recovery must be in [0, 1)");
        std::sort(quotes.begin(), quotes.end(),
                  [](const CdsQuote &a, const CdsQuote &b)
                  { return a.maturity < b.maturity; });

        std::vector<Time> times;
        std::vector<Real> hazards;
        for (const CdsQuote &q : quotes)
        {
            if (!(q.maturity > 0.0) || !(q.spread > 0.0))
                throw InvalidInput("bootstrap_credit_curve: maturities and spreads must be > 0");
            if (!times.empty() && !(q.maturity > times.back()))
                throw InvalidInput("bootstrap_credit_curve: duplicate maturity t=" +
                                   std::to_string(q.maturity));

            const CreditDefaultSwap cds = make_cds(q.maturity, q.spread, frequency);
            // λ is an annual default intensity: 50 is a default within days.
            Real lo = 0.0, hi = 50.0;
            if (buyer_value(times, hazards, cds, discount, recovery, lo) > 0.0)
                throw InvalidInput(
                    "bootstrap_credit_curve: the " + std::to_string(q.spread * 1e4) +
                    "bp quote at t=" + std::to_string(q.maturity) +
                    " is below what the earlier quotes already imply with zero default risk "
                    "on its segment (the spread curve falls too steeply: arbitrage)");
            if (buyer_value(times, hazards, cds, discount, recovery, hi) < 0.0)
                throw InvalidInput("bootstrap_credit_curve: no hazard rate reaches the quote at t=" +
                                   std::to_string(q.maturity));
            for (int iter = 0; iter < 100 && hi - lo > 1e-14; ++iter)
            {
                const Real mid = 0.5 * (lo + hi);
                if (buyer_value(times, hazards, cds, discount, recovery, mid) < 0.0)
                    lo = mid;
                else
                    hi = mid;
            }
            times.push_back(q.maturity);
            hazards.push_back(0.5 * (lo + hi));
        }
        return CreditCurve(std::move(times), std::move(hazards));
    }

    Real flat_hazard_from_spread(Time maturity, Real spread, const DiscountCurve &discount,
                                 Real recovery, int frequency)
    {
        return bootstrap_credit_curve({{maturity, spread}}, discount, recovery, frequency).hazards().front();
    }

} // namespace quantModeling
