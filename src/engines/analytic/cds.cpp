#include "quantModeling/engines/analytic/cds.hpp"

#include "quantModeling/models/credit/intensity.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{
    namespace
    {
        void validate(const CreditDefaultSwap &cds)
        {
            if (cds.payment_times.empty() || cds.payment_times.size() != cds.accruals.size())
                throw InvalidInput("CDS: payment_times and accruals must match and be non-empty");
            Time previous = 0.0;
            for (std::size_t i = 0; i < cds.payment_times.size(); ++i)
            {
                if (!(cds.payment_times[i] > previous))
                    throw InvalidInput("CDS: payment times must be > 0 and strictly increasing");
                if (!(cds.accruals[i] > 0.0))
                    throw InvalidInput("CDS: accruals must be > 0");
                previous = cds.payment_times[i];
            }
        }

        /// (1 - e^{-kΔ}) / k, stable as k → 0.
        Real survival_integral(Real k, Time delta)
        {
            const Real x = k * delta;
            if (std::fabs(x) < 1e-6)
                return delta * (1.0 - 0.5 * x + x * x / 6.0);
            return -std::expm1(-x) / k;
        }

        /// (1 - e^{-kΔ}(1 + kΔ)) / k² = ∫_0^Δ s e^{-ks} ds, stable as k → 0.
        Real first_moment_integral(Real k, Time delta)
        {
            const Real x = k * delta;
            if (std::fabs(x) < 1e-4)
                return delta * delta * (0.5 - x / 3.0 + x * x / 8.0);
            return (1.0 - std::exp(-x) * (1.0 + x)) / (k * k);
        }
    } // namespace

    CreditDefaultSwap make_cds(Time maturity, Real spread, int frequency, Real notional,
                               bool protection_buyer)
    {
        if (!(maturity > 0.0))
            throw InvalidInput("make_cds: maturity must be > 0");
        if (frequency < 1)
            throw InvalidInput("make_cds: frequency must be >= 1");
        // Roll back from maturity, as a CDS schedule does: any short period
        // is the first one. A remainder under a day is absorbed, not kept as
        // a sliver of a period.
        const Time period = 1.0 / static_cast<Real>(frequency);
        std::vector<Time> times;
        for (Time t = maturity; t > 1.0 / 365.0; t -= period)
            times.push_back(t);
        std::reverse(times.begin(), times.end());

        CreditDefaultSwap cds;
        cds.spread = spread;
        cds.notional = notional;
        cds.protection_buyer = protection_buyer;
        cds.payment_times = times;
        Time previous = 0.0;
        for (const Time t : times)
        {
            cds.accruals.push_back(t - previous);
            previous = t;
        }
        return cds;
    }

    CdsLegs cds_legs(const CreditDefaultSwap &cds, const DiscountCurve &discount,
                     const CreditCurve &credit, Real recovery)
    {
        validate(cds);
        if (!(recovery >= 0.0 && recovery < 1.0))
            throw InvalidInput("cds_legs: recovery must be in [0, 1)");

        // Every point where λ or f may change: pieces between two of them
        // have both constant.
        std::vector<Time> knots = credit.times();
        knots.insert(knots.end(), discount.pillar_times().begin(), discount.pillar_times().end());
        std::sort(knots.begin(), knots.end());
        knots.erase(std::unique(knots.begin(), knots.end()), knots.end());

        CdsLegs legs;
        Real protection = 0.0;
        Time period_start = 0.0;
        for (std::size_t i = 0; i < cds.payment_times.size(); ++i)
        {
            const Time period_end = cds.payment_times[i];
            const Real accrual = cds.accruals[i];
            legs.risky_annuity += accrual * discount.discount(period_end) * credit.survival(period_end);

            // Default in (a, b] costs the premium accrued since period_start,
            // accrual · (τ - period_start) / (period_end - period_start).
            const Real accrual_rate = accrual / (period_end - period_start);
            Time a = period_start;
            auto next = std::upper_bound(knots.begin(), knots.end(), a);
            while (a < period_end)
            {
                const Time b = (next != knots.end() && *next < period_end) ? *next++ : period_end;
                const Time delta = b - a;
                const Real survival_a = credit.survival(a);
                // Right limit at a: DiscountCurve is flat before its first
                // pillar (DF(0+) = DF(t_1), a deliberate convention), and the
                // legs follow the curve's own rule rather than a second one.
                const Real df_a = discount.discount(a > 0.0 ? a : std::nextafter(0.0, 1.0));
                const Real lambda = credit.hazard(0.5 * (a + b));
                const Real f = -std::log(discount.discount(b) / df_a) / delta;
                const Real k = lambda + f;
                const Real weight = survival_a * df_a * lambda;
                const Real i0 = survival_integral(k, delta);
                protection += weight * i0;
                legs.accrued_on_default +=
                    accrual_rate * weight * ((a - period_start) * i0 + first_moment_integral(k, delta));
                a = b;
            }
            period_start = period_end;
        }
        legs.risky_annuity += legs.accrued_on_default;
        legs.protection = (1.0 - recovery) * protection;
        return legs;
    }

    Real cds_npv(const CreditDefaultSwap &cds, const CdsLegs &legs)
    {
        const Real buyer = legs.protection - cds.spread * legs.risky_annuity;
        return cds.notional * (cds.protection_buyer ? buyer : -buyer);
    }

    void CdsAnalyticEngine::visit(const CreditDefaultSwap &cds)
    {
        const auto &m = require_model<IIntensityModel>("CdsAnalyticEngine");
        const CdsLegs legs = cds_legs(cds, m.discount_curve(), m.credit_curve(), m.recovery());
        PricingResult out;
        out.npv = cds_npv(cds, legs);
        out.diagnostics = "Analytic CDS (ISDA standard model integrals), par spread " +
                          std::to_string(legs.par_spread() * 1e4) + "bp";
        res_ = out;
    }

} // namespace quantModeling
