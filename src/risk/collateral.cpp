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
        constexpr std::size_t kToday = static_cast<std::size_t>(-1);
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

        void require_margin_matrix(const std::vector<Real> &matrix, std::size_t cells, const char *name)
        {
            if (matrix.empty())
                return;
            if (matrix.size() != cells)
                throw InvalidInput(std::string("collateral: the path-dependent ") + name +
                                   " needs one value per path and reporting date");
            for (const Real im : matrix)
                if (!(im >= 0.0))
                    throw InvalidInput(std::string("collateral: ") + name + " must be >= 0");
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
    } // namespace

    Real required_variation_margin(Real value, const Csa &csa)
    {
        return variation_margin_required(value, CollateralTerms::of(csa));
    }

    Real collateral_after_call(Real held, Real required, const Csa &csa)
    {
        return balance_after_call(held, required, CollateralTerms::of(csa));
    }

    CollateralPlan collateral_plan(const std::vector<Time> &times, const Csa &csa, Real value_today,
                                   MarginPeriodCashflows cashflows)
    {
        csa.validate();
        const std::size_t n = times.size();
        if (n == 0)
            throw InvalidInput("collateral: the simulation is empty");
        const std::vector<std::size_t> lagged = lagged_dates(times, csa.margin_period_of_risk);
        // The first dates always report (their lagged date is today), so the
        // test is on the last one: a grid built without the lagged dates
        // would silently report the first days only.
        if (lagged[n - 1] == kMissing)
            throw InvalidInput(
                "collateral: the last date of the simulation has no lagged date t - MPoR on the "
                "grid; simulate with grid.margin_period_of_risk equal to the CSA's");
        CollateralPlan plan;
        plan.terms = CollateralTerms::of(csa);
        plan.cashflows = cashflows;
        plan.lagged.resize(n);
        // The dates at which a margin call is observed: the lagged dates.
        plan.is_call_date.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
        {
            plan.lagged[i] = lagged[i] == kMissing ? CollateralPlan::kMissing
                             : lagged[i] == kToday ? CollateralPlan::kToday
                                                   : static_cast<int>(lagged[i]);
            if (lagged[i] == kMissing)
                continue;
            plan.reporting.push_back(static_cast<int>(i));
            if (lagged[i] != kToday)
                plan.is_call_date[lagged[i]] = 1;
        }
        plan.held_today =
            balance_after_call(0.0, variation_margin_required(value_today, plan.terms), plan.terms);
        return plan;
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

        Real value_today = 0.0;
        for (const std::size_t k : netted_trades)
            value_today += paths.trade_values_today[k];
        const CollateralPlan plan = collateral_plan(paths.times, csa, value_today, settings.cashflows);
        const CollateralPlanView dates = view(plan);
        const std::vector<int> &reporting = plan.reporting;
        const std::size_t m = reporting.size();
        const Real held_today = plan.held_today;
        require_margin_profile(settings.initial_margin_received, m, "initial margin received");
        require_margin_profile(settings.initial_margin_posted, m, "initial margin posted");
        require_margin_matrix(settings.initial_margin_received_paths, N * m, "initial margin received");
        require_margin_matrix(settings.initial_margin_posted_paths, N * m, "initial margin posted");

        ExposurePaths out;
        out.paths = N;
        out.measure = paths.measure;
        out.times.reserve(m);
        out.discount.reserve(m);
        for (const int i : reporting)
        {
            out.times.push_back(paths.times[i]);
            out.discount.push_back(paths.discount[i]);
        }
        out.discount_weight.resize(N * m);
        out.trade_values.assign(1, std::vector<Real>(N * m));
        // Margin in place on path p at reporting date r: the path's own when
        // a matrix was given, the profile's otherwise.
        const auto margin = [m](const std::vector<Real> &matrix, const std::vector<Real> &profile,
                                std::size_t p, std::size_t r)
        {
            if (!matrix.empty())
                return matrix[p * m + r];
            return profile.empty() ? 0.0 : profile[r];
        };
        out.trade_values_today = {net_of_initial_margin(
            value_today - held_today - csa.independent_amount,
            margin(settings.initial_margin_received_paths, settings.initial_margin_received, 0, 0),
            margin(settings.initial_margin_posted_paths, settings.initial_margin_posted, 0, 0))};

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

            collateral_balances(dates, value.data(), held.data());
            for (std::size_t r = 0; r < m; ++r)
            {
                const std::size_t i = static_cast<std::size_t>(reporting[r]);
                const Real exposure = collateralised_value(dates, static_cast<int>(r), value.data(),
                                                           flow.data(), held.data());
                out.trade_values[0][p * m + r] = net_of_initial_margin(
                    exposure,
                    margin(settings.initial_margin_received_paths, settings.initial_margin_received, p, r),
                    margin(settings.initial_margin_posted_paths, settings.initial_margin_posted, p, r));
                out.discount_weight[p * m + r] = paths.discount_weight[row + i];
            }
        }
        return out;
    }

    std::vector<MarginPeriod> margin_periods(const std::vector<Time> &times,
                                             Time margin_period_of_risk)
    {
        const std::vector<std::size_t> lagged = lagged_dates(times, margin_period_of_risk);
        std::vector<MarginPeriod> out;
        for (std::size_t i = 0; i < times.size(); ++i)
            if (lagged[i] != kMissing)
                out.push_back({i, lagged[i] == kToday ? MarginPeriod::kToday : lagged[i]});
        return out;
    }

    std::vector<Real> discounted_expected_collateral(const ExposurePaths &paths, const Csa &csa,
                                                     const std::vector<std::size_t> &trades)
    {
        // V - C is the collateralised value: the collateral is what was taken
        // off the value, read on the same recursion.
        const ExposurePaths net = collateralise(paths, csa, trades, {});
        const std::vector<std::size_t> reporting =
            collateral_reporting_dates(paths, csa.margin_period_of_risk);
        const std::size_t n = paths.dates(), N = paths.paths, m = reporting.size();
        std::vector<std::size_t> set = trades;
        if (set.empty())
        {
            set.resize(paths.trades());
            std::iota(set.begin(), set.end(), std::size_t{0});
        }
        std::vector<Real> profile(m, 0.0);
        for (std::size_t r = 0; r < m; ++r)
        {
            const std::size_t i = reporting[r];
            Real sum = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                Real value = 0.0;
                for (const std::size_t k : set)
                    value += paths.trade_values[k][p * n + i];
                sum += net.discount_weight[p * m + r] * (value - net.trade_values[0][p * m + r]);
            }
            profile[r] = sum / static_cast<Real>(N);
        }
        return profile;
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
