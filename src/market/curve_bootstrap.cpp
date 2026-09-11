#include "quantModeling/market/curve_bootstrap.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{
    namespace
    {
        /// Log-linear DF interpolation, flat before the first pillar and past
        /// the last one — bit-for-bit the same rule DiscountCurve::discount()
        /// uses (including the flat-left convention, which is a deliberate
        /// choice there, not an oversight — see DiscountCurve's own tests).
        /// A bootstrap that solved each pillar under a *different* rule for
        /// its own short coupons would converge to discount factors that this
        /// curve then reprices inconsistently — exactly the failure the
        /// RepricingASemiannualBondReturnsPar test would have caught.
        Real interp_df(const std::vector<Time> &times, const std::vector<Real> &dfs,
                       Time t)
        {
            if (t <= 0.0 || times.empty())
                return 1.0;
            if (t <= times.front())
                return dfs.front();
            if (t >= times.back())
                return dfs.back();

            const auto it = std::upper_bound(times.begin(), times.end(), t);
            const std::size_t idx =
                static_cast<std::size_t>(std::distance(times.begin(), it));
            const Time t1 = times[idx - 1], t2 = times[idx];
            const Real df1 = dfs[idx - 1], df2 = dfs[idx];
            const Real w = (t - t1) / (t2 - t1);
            return std::exp((1.0 - w) * std::log(df1) + w * std::log(df2));
        }

        /// The par-rate equation evaluated at a trial continuously-compounded
        /// zero rate `z` for the new pillar at q.maturity(): positive when `z`
        /// is too low (the instrument would be worth more than par), negative
        /// when too high. Every coupon strictly before maturity is priced off
        /// the *candidate* curve (existing pillars + this trial point), which
        /// is what makes an instrument whose schedule has no pillar of its own
        /// between the previous maturity and this one (e.g. a 5Y quote after a
        /// 3Y one, with coupons at 3.5Y/4Y/4.5Y) solvable at all.
        Real par_equation(const std::vector<Time> &times, const std::vector<Real> &dfs,
                          const ParRateQuote &q, Real z)
        {
            const Time T = q.maturity();
            const Real df_new = std::exp(-z * T);

            std::vector<Time> cand_times = times;
            std::vector<Real> cand_dfs = dfs;
            cand_times.push_back(T);
            cand_dfs.push_back(df_new);

            Real coupon_pv = 0.0;
            for (std::size_t i = 0; i < q.payment_times.size(); ++i)
            {
                const Real df_i = (i + 1 == q.payment_times.size())
                                      ? df_new
                                      : interp_df(cand_times, cand_dfs,
                                                  q.payment_times[i]);
                coupon_pv += q.accruals[i] * df_i;
            }
            return q.rate * coupon_pv + df_new - 1.0;
        }

        /// Bisect par_equation(..., z) = 0 on a generous bracket. The equation
        /// is strictly decreasing in z (a higher discount rate can only lower
        /// every cash flow's present value), so a sign change brackets a
        /// unique root.
        Real solve_zero_rate(const std::vector<Time> &times,
                             const std::vector<Real> &dfs, const ParRateQuote &q)
        {
            Real lo = -1.0, hi = 3.0;
            const Real f_lo = par_equation(times, dfs, q, lo);
            const Real f_hi = par_equation(times, dfs, q, hi);
            if (!(f_lo > 0.0 && f_hi < 0.0))
                throw InvalidInput(
                    "bootstrap_curve: could not bracket a root for the quote "
                    "maturing at t=" +
                    std::to_string(q.maturity()) +
                    " (rate far outside what an arbitrage-free curve can "
                    "produce from the pillars before it)");

            Real z = 0.0;
            for (int iter = 0; iter < 100; ++iter)
            {
                z = 0.5 * (lo + hi);
                if (par_equation(times, dfs, q, z) > 0.0)
                    lo = z;
                else
                    hi = z;
            }
            return z;
        }
    } // namespace

    ParRateQuote make_semiannual_bond_quote(Time maturity, Real par_yield)
    {
        if (!(maturity > 0.0))
            throw InvalidInput("make_semiannual_bond_quote: maturity must be > 0");

        const Real periods_f = maturity / 0.5;
        const auto periods = static_cast<int>(std::lround(periods_f));
        if (periods < 1 || std::fabs(periods_f - static_cast<Real>(periods)) > 1e-6)
            throw InvalidInput(
                "make_semiannual_bond_quote: maturity must be a whole number "
                "of half-years (got " +
                std::to_string(maturity) + ")");

        ParRateQuote q;
        q.rate = par_yield;
        q.payment_times.reserve(static_cast<std::size_t>(periods));
        q.accruals.assign(static_cast<std::size_t>(periods), 0.5);
        for (int i = 1; i <= periods; ++i)
            q.payment_times.push_back(0.5 * static_cast<Real>(i));
        return q;
    }

    DiscountCurve bootstrap_curve(const std::vector<DepositQuote> &deposits_in,
                                  const std::vector<ParRateQuote> &par_quotes_in)
    {
        if (deposits_in.empty() && par_quotes_in.empty())
            throw InvalidInput("bootstrap_curve: need at least one quote");

        for (const DepositQuote &d : deposits_in)
            if (!(d.maturity > 0.0))
                throw InvalidInput("bootstrap_curve: deposit maturity must be > 0");
        for (const ParRateQuote &q : par_quotes_in)
        {
            if (q.payment_times.empty() ||
                q.payment_times.size() != q.accruals.size())
                throw InvalidInput(
                    "bootstrap_curve: a par-rate quote needs matching, "
                    "non-empty payment_times and accruals");
            if (!(q.maturity() > 0.0))
                throw InvalidInput("bootstrap_curve: par-rate maturity must be > 0");
        }

        std::vector<DepositQuote> deposits = deposits_in;
        std::vector<ParRateQuote> par_quotes = par_quotes_in;
        std::sort(deposits.begin(), deposits.end(),
                  [](const DepositQuote &a, const DepositQuote &b)
                  { return a.maturity < b.maturity; });
        std::sort(par_quotes.begin(), par_quotes.end(),
                  [](const ParRateQuote &a, const ParRateQuote &b)
                  { return a.maturity() < b.maturity(); });

        std::vector<Time> times;
        std::vector<Real> dfs;
        times.reserve(deposits.size() + par_quotes.size());
        dfs.reserve(times.capacity());

        std::size_t di = 0, pi = 0;
        while (di < deposits.size() || pi < par_quotes.size())
        {
            const bool take_deposit =
                pi >= par_quotes.size() ||
                (di < deposits.size() &&
                 deposits[di].maturity <= par_quotes[pi].maturity());

            const Time t = take_deposit ? deposits[di].maturity
                                        : par_quotes[pi].maturity();
            if (!times.empty() && t <= times.back())
                throw InvalidInput(
                    "bootstrap_curve: quote maturities must be strictly "
                    "increasing once merged and sorted (duplicate at t=" +
                    std::to_string(t) + ")");

            if (take_deposit)
            {
                const DepositQuote &d = deposits[di++];
                times.push_back(d.maturity);
                dfs.push_back(1.0 / (1.0 + d.rate * d.maturity));
            }
            else
            {
                const ParRateQuote &q = par_quotes[pi++];
                const Real z = solve_zero_rate(times, dfs, q);
                times.push_back(q.maturity());
                dfs.push_back(std::exp(-z * q.maturity()));
            }
        }

        return DiscountCurve(times, dfs);
    }

    Real t_bill_bond_equivalent_yield(Real discount_rate, int days)
    {
        if (days <= 0)
            throw InvalidInput("t_bill_bond_equivalent_yield: days must be > 0");
        if (days > 182)
            throw InvalidInput(
                "t_bill_bond_equivalent_yield: the simple <=182-day formula "
                "does not apply beyond 6 months");
        return 365.0 * discount_rate / (360.0 - discount_rate * static_cast<Real>(days));
    }

} // namespace quantModeling
