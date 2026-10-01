#include "quantModeling/risk/xva.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        void require_grid(const std::vector<Time> &times, std::size_t values)
        {
            if (times.empty() || times.size() != values)
                throw InvalidInput("xVA profile requires matching non-empty times and values");
            Time previous = -1.0;
            for (const Time t : times)
            {
                if (!(t >= 0.0) || !(t > previous))
                    throw InvalidInput("xVA profile times must be >= 0 and strictly increasing");
                previous = t;
            }
        }

        void require_lgd(Real lgd)
        {
            if (!(lgd >= 0.0 && lgd <= 1.0))
                throw InvalidInput("loss given default must be in [0, 1]");
        }

        /// -LGD Σ_i exposure(t_i) S_other(t_{i-1}) PD_defaulter(t_{i-1}, t_i):
        /// the first-default sum behind both CVA and DVA.
        Real default_adjustment(const std::vector<Time> &times, const std::vector<Real> &exposure,
                                const CreditCurve &defaulter, const CreditCurve *other, Real lgd)
        {
            require_lgd(lgd);
            Real sum = 0.0;
            Time previous = 0.0;
            for (std::size_t i = 0; i < times.size(); ++i)
            {
                const Real pd = defaulter.survival(previous) - defaulter.survival(times[i]);
                const Real survival_other = other ? other->survival(previous) : 1.0;
                sum += exposure[i] * survival_other * pd;
                previous = times[i];
            }
            return -lgd * sum;
        }
    } // namespace

    std::vector<Real> ExposureProfile::discounted_efv() const
    {
        validate();
        std::vector<Real> efv(times.size());
        for (std::size_t i = 0; i < times.size(); ++i)
            efv[i] = discounted_ee[i] + discounted_ene[i];
        return efv;
    }

    ExposureProfile ExposureProfile::seen_from_counterparty() const
    {
        validate();
        ExposureProfile mirrored;
        mirrored.times = times;
        mirrored.discounted_ee.reserve(times.size());
        mirrored.discounted_ene.reserve(times.size());
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            mirrored.discounted_ee.push_back(-discounted_ene[i]);
            mirrored.discounted_ene.push_back(-discounted_ee[i]);
        }
        return mirrored;
    }

    void ExposureProfile::validate() const
    {
        require_grid(times, discounted_ee.size());
        if (discounted_ene.size() != times.size())
            throw InvalidInput("xVA profile requires matching non-empty times and values");
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            if (!(discounted_ee[i] >= 0.0))
                throw InvalidInput("expected exposure must be >= 0");
            if (!(discounted_ene[i] <= 0.0))
                throw InvalidInput("expected negative exposure must be <= 0 (Gregory's convention)");
        }
    }

    Real cva_unilateral(const ExposureProfile &profile, const CreditCurve &counterparty, Real lgd)
    {
        profile.validate();
        return default_adjustment(profile.times, profile.discounted_ee, counterparty, nullptr, lgd);
    }

    Real cva_bilateral(const ExposureProfile &profile, const CreditCurve &counterparty,
                       const CreditCurve &own, Real lgd_counterparty)
    {
        profile.validate();
        return default_adjustment(profile.times, profile.discounted_ee, counterparty, &own,
                                  lgd_counterparty);
    }

    Real dva(const ExposureProfile &profile, const CreditCurve &counterparty, const CreditCurve &own,
             Real lgd_own)
    {
        profile.validate();
        return default_adjustment(profile.times, profile.discounted_ene, own, &counterparty,
                                  lgd_own);
    }

    Real cva_spread_approximation(Real credit_spread, Real epe, Time maturity)
    {
        if (!(credit_spread >= 0.0) || !(epe >= 0.0) || !(maturity >= 0.0))
            throw InvalidInput("CVA spread approximation: spread, EPE and maturity must be >= 0");
        return -credit_spread * epe * maturity;
    }

    Real hazard_from_spread(Real credit_spread, Real lgd)
    {
        if (!(credit_spread >= 0.0) || !(lgd > 0.0 && lgd <= 1.0))
            throw InvalidInput("credit triangle: spread must be >= 0 and LGD in (0, 1]");
        return credit_spread / lgd;
    }

    Real running_cost_adjustment(const std::vector<Time> &times,
                                 const std::vector<Real> &discounted_profile,
                                 const CreditCurve &counterparty, const CreditCurve &own, Real rate)
    {
        require_grid(times, discounted_profile.size());
        if (!std::isfinite(rate))
            throw InvalidInput("xVA running cost rate must be finite");
        Real sum = 0.0;
        Time previous = 0.0;
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            sum += discounted_profile[i] * counterparty.survival(times[i]) *
                   own.survival(times[i]) * (times[i] - previous);
            previous = times[i];
        }
        return -rate * sum;
    }

    Real fca(const ExposureProfile &profile, const CreditCurve &counterparty, const CreditCurve &own,
             Real borrowing_spread)
    {
        profile.validate();
        return running_cost_adjustment(profile.times, profile.discounted_ee, counterparty, own,
                                       borrowing_spread);
    }

    Real fba(const ExposureProfile &profile, const CreditCurve &counterparty, const CreditCurve &own,
             Real lending_spread)
    {
        profile.validate();
        return running_cost_adjustment(profile.times, profile.discounted_ene, counterparty, own,
                                       lending_spread);
    }

    Real fva(const ExposureProfile &profile, const CreditCurve &counterparty, const CreditCurve &own,
             Real borrowing_spread, Real lending_spread)
    {
        return fca(profile, counterparty, own, borrowing_spread) +
               fba(profile, counterparty, own, lending_spread);
    }

    Real mva(const std::vector<Time> &times, const std::vector<Real> &discounted_expected_im,
             const CreditCurve &counterparty, const CreditCurve &own,
             Real funding_spread_over_im_return)
    {
        for (const Real im : discounted_expected_im)
            if (!(im >= 0.0))
                throw InvalidInput("posted initial margin must be >= 0");
        return running_cost_adjustment(times, discounted_expected_im, counterparty, own,
                                       funding_spread_over_im_return);
    }

    Real kva(const std::vector<Time> &times, const std::vector<Real> &discounted_expected_capital,
             const CreditCurve &counterparty, const CreditCurve &own, Real cost_of_capital)
    {
        for (const Real capital : discounted_expected_capital)
            if (!(capital >= 0.0))
                throw InvalidInput("regulatory capital must be >= 0");
        return running_cost_adjustment(times, discounted_expected_capital, counterparty, own,
                                       cost_of_capital);
    }

} // namespace quantModeling
