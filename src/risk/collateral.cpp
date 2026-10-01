#include "quantModeling/risk/collateral.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace quantModeling
{
    namespace
    {
        /// Two grid dates are the same date within this tolerance.
        constexpr Real kTimeEps = 1e-9;
        /// The lagged date of a reporting date is today (or earlier).
        constexpr std::size_t kToday = static_cast<std::size_t>(-1);
        /// The lagged date is not on the grid: not a reporting date.
        constexpr std::size_t kMissing = static_cast<std::size_t>(-2);

        /// For each date, the index of t - MPoR on the grid.
        std::vector<std::size_t> lagged_dates(const std::vector<Time> &times, Time mpor)
        {
            std::vector<std::size_t> lagged(times.size(), kMissing);
            for (std::size_t i = 0; i < times.size(); ++i)
            {
                if (mpor <= kTimeEps)
                {
                    lagged[i] = i;
                    continue;
                }
                const Time target = times[i] - mpor;
                if (target <= kTimeEps)
                {
                    lagged[i] = kToday;
                    continue;
                }
                const auto it = std::lower_bound(times.begin(), times.end(), target - kTimeEps);
                if (it != times.end() && std::abs(*it - target) <= kTimeEps)
                    lagged[i] = static_cast<std::size_t>(it - times.begin());
            }
            return lagged;
        }

        void require_margin_profile(const std::vector<Real> &profile, std::size_t dates,
                                    const char *name)
        {
            if (profile.empty())
                return;
            if (profile.size() != dates)
                throw InvalidInput(std::string("collateral: the ") + name +
                                   " profile needs one value per reporting date");
            for (const Real im : profile)
                if (!(im >= 0.0))
                    throw InvalidInput(std::string("collateral: ") + name + " must be >= 0");
        }

        /// Value net of initial margin on both sides.
        Real after_initial_margin(Real value, Real received, Real posted)
        {
            if (value > received)
                return value - received;
            if (value < -posted)
                return value + posted;
            return 0.0;
        }
    } // namespace

    Real required_variation_margin(Real value, const Csa &csa)
    {
        return std::max(value - csa.threshold_counterparty, 0.0) -
               std::max(-value - csa.threshold_bank, 0.0);
    }

    Real collateral_after_call(Real held, Real required, const Csa &csa)
    {
        Real transfer = required - held;
        // Strictly below the MTA: no transfer. A zero transfer is none either.
        if (transfer == 0.0 || std::abs(transfer) < csa.minimum_transfer_amount)
            return held;
        if (csa.rounding > 0.0)
            transfer = std::round(transfer / csa.rounding) * csa.rounding;
        return held + transfer;
    }

    std::vector<std::size_t> collateral_reporting_dates(const ExposurePaths &paths,
                                                        Time margin_period_of_risk)
    {
        if (!(margin_period_of_risk >= 0.0))
            throw InvalidInput("CSA: the margin period of risk must be >= 0");
        const std::vector<std::size_t> lagged = lagged_dates(paths.times, margin_period_of_risk);
        std::vector<std::size_t> reporting;
        for (std::size_t i = 0; i < lagged.size(); ++i)
            if (lagged[i] != kMissing)
                reporting.push_back(i);
        return reporting;
    }

    ExposurePaths collateralise(const ExposurePaths &paths, const Csa &csa,
                                const std::vector<std::size_t> &trades,
                                const CollateralSettings &settings)
    {
        csa.validate();
        const std::size_t n = paths.dates();
        const std::size_t N = paths.paths;
        if (n == 0 || N == 0 || paths.trades() == 0)
            throw InvalidInput("collateral: the simulation is empty");

        std::vector<std::size_t> netted_trades = trades;
        if (netted_trades.empty())
        {
            netted_trades.resize(paths.trades());
            std::iota(netted_trades.begin(), netted_trades.end(), std::size_t{0});
        }
        for (const std::size_t k : netted_trades)
            if (k >= paths.trades())
                throw InvalidInput("collateral: trade index out of range");

        const bool needs_cashflows = settings.cashflows != MarginPeriodCashflows::Paid;
        if (needs_cashflows && paths.trade_cashflows.size() != paths.trades())
            throw InvalidInput("collateral: this treatment of the margin period needs the cash "
                               "flows; simulate with keep_cashflows = true");

        const std::vector<std::size_t> lagged = lagged_dates(paths.times, csa.margin_period_of_risk);
        std::vector<std::size_t> reporting;
        for (std::size_t i = 0; i < n; ++i)
            if (lagged[i] != kMissing)
                reporting.push_back(i);
        // The first dates always report (their lagged date is today), so the
        // test is on the last one: a grid built without the lagged dates
        // would silently report the first days only.
        if (lagged[n - 1] == kMissing)
            throw InvalidInput(
                "collateral: the last date of the simulation has no lagged date t - MPoR on the "
                "grid; simulate with grid.margin_period_of_risk equal to the CSA's");
        const std::size_t m = reporting.size();
        require_margin_profile(settings.initial_margin_received, m, "initial margin received");
        require_margin_profile(settings.initial_margin_posted, m, "initial margin posted");

        // The dates at which a margin call is observed: the lagged dates.
        std::vector<bool> is_call_date(n, false);
        for (const std::size_t i : reporting)
            if (lagged[i] != kToday)
                is_call_date[lagged[i]] = true;

        Real value_today = 0.0;
        for (const std::size_t k : netted_trades)
            value_today += paths.trade_values_today[k];
        const Real held_today =
            collateral_after_call(0.0, required_variation_margin(value_today, csa), csa);

        ExposurePaths out;
        out.paths = N;
        out.measure = paths.measure;
        out.times.reserve(m);
        out.discount.reserve(m);
        for (const std::size_t i : reporting)
        {
            out.times.push_back(paths.times[i]);
            out.discount.push_back(paths.discount[i]);
        }
        out.discount_weight.resize(N * m);
        out.trade_values.assign(1, std::vector<Real>(N * m));
        const auto margin = [](const std::vector<Real> &profile, std::size_t r)
        { return profile.empty() ? 0.0 : profile[r]; };
        out.trade_values_today = {after_initial_margin(
            value_today - held_today - csa.independent_amount,
            margin(settings.initial_margin_received, 0), margin(settings.initial_margin_posted, 0))};

        std::vector<Real> value(n), flow(n), held(n);
        for (std::size_t p = 0; p < N; ++p)
        {
            const std::size_t row = p * n;
            std::fill(value.begin(), value.end(), 0.0);
            std::fill(flow.begin(), flow.end(), 0.0);
            for (const std::size_t k : netted_trades)
            {
                const std::vector<Real> &values = paths.trade_values[k];
                for (std::size_t i = 0; i < n; ++i)
                    value[i] += values[row + i];
                if (needs_cashflows)
                {
                    // Payment netting: the flows of a date settle for their sum.
                    const std::vector<Real> &flows = paths.trade_cashflows[k];
                    for (std::size_t i = 0; i < n; ++i)
                        flow[i] += flows[row + i];
                }
            }

            // The balance is only updated when a call is observed; the
            // minimum transfer amount makes it depend on the whole path.
            Real balance = held_today;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!is_call_date[i])
                    continue;
                balance = collateral_after_call(balance, required_variation_margin(value[i], csa), csa);
                held[i] = balance;
            }

            for (std::size_t r = 0; r < m; ++r)
            {
                const std::size_t i = reporting[r];
                const Real collateral =
                    (lagged[i] == kToday ? held_today : held[lagged[i]]) + csa.independent_amount;
                Real exposure = value[i] - collateral;
                if (needs_cashflows)
                {
                    // Flows due in (t - MPoR, t] that were not exchanged stay
                    // in the close-out amount.
                    const std::size_t first = lagged[i] == kToday ? 0 : lagged[i] + 1;
                    for (std::size_t j = first; j <= i; ++j)
                        exposure += settings.cashflows == MarginPeriodCashflows::Withheld
                                        ? flow[j]
                                        : std::max(flow[j], 0.0);
                }
                out.trade_values[0][p * m + r] =
                    after_initial_margin(exposure, margin(settings.initial_margin_received, r),
                                         margin(settings.initial_margin_posted, r));
                out.discount_weight[p * m + r] = paths.discount_weight[row + i];
            }
        }
        return out;
    }

    std::vector<Real> deterministic_initial_margin(Real im_today, const std::vector<Time> &times,
                                                   Time maturity)
    {
        if (!(im_today >= 0.0) || !(maturity > 0.0))
            throw InvalidInput("initial margin: need im_today >= 0 and maturity > 0");
        std::vector<Real> profile;
        profile.reserve(times.size());
        for (const Time t : times)
            profile.push_back(im_today * std::sqrt(std::max(maturity - t, 0.0) / maturity));
        return profile;
    }

} // namespace quantModeling
