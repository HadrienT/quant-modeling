#include "quantModeling/market/raw_vol_surface.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace quantModeling
{

    Real RawOptionQuote::mid() const noexcept
    {
        if (bid > 0.0 && ask > 0.0)
            return 0.5 * (bid + ask);
        return last;
    }

    namespace
    {

        bool passes_liquidity(const RawOptionQuote &q, const CleaningParams &p, bool oi_data_available)
        {
            if (oi_data_available)
            {
                if (q.open_interest < p.min_open_interest)
                    return false;
            }
            else
            {
                if (q.volume < p.min_open_interest)
                    return false;
            }

            if (!q.has_iv || q.implied_vol <= 0.0)
                return false;

            const Real mid = q.mid();
            if (mid <= 0.0)
                return false;

            if (q.bid == 0.0 && q.ask == 0.0)
                return true; // no bid/ask reported: rely on mid (from last) and volume/OI already checked

            if (q.bid < p.min_bid)
                return false;

            const Real spread = q.ask - q.bid;
            if (mid > 0.0 && spread / mid > p.max_spread_ratio)
                return false;

            return true;
        }

        bool passes_moneyness(const RawOptionQuote &q, Real spot, Real lo, Real hi)
        {
            const Real m = q.strike / spot;
            return lo <= m && m <= hi;
        }

        /// Per (is_call, strike rounded to the nearest 0.5) series, sorted by
        /// TTM ascending: keep a quote only while its total variance w = iv^2*T
        /// is non-decreasing, i.e. drop anything that would make total
        /// variance decrease with maturity (calendar arbitrage).
        std::vector<RawOptionQuote> remove_calendar_arbitrage(std::vector<RawOptionQuote> quotes)
        {
            std::map<std::pair<bool, long long>, std::vector<RawOptionQuote>> buckets;
            for (auto &q : quotes)
            {
                const long long half_steps = std::llround(q.strike * 2.0);
                buckets[{q.is_call, half_steps}].push_back(q);
            }

            std::vector<RawOptionQuote> clean;
            for (auto &[key, series] : buckets)
            {
                std::sort(series.begin(), series.end(),
                          [](const RawOptionQuote &a, const RawOptionQuote &b)
                          { return a.ttm < b.ttm; });

                Real max_w = -1.0;
                for (const auto &q : series)
                {
                    const Real w = q.implied_vol * q.implied_vol * q.ttm;
                    if (w >= max_w)
                    {
                        max_w = w;
                        clean.push_back(q);
                    }
                    // else: calendar arbitrage — drop silently, as the Python
                    // pipeline this was ported from does.
                }
            }
            return clean;
        }

        /// Discrete convexity in strike, calls only (put-call parity gives the
        /// same surface from puts): for consecutive strikes K1 < K2 < K3,
        /// C(K1) - 2*C(K2) + C(K3) must be >= 0. Iteratively drop the middle
        /// quote of any violating triple, per expiry (grouped by exact TTM —
        /// quotes for one expiry share one TTM value by construction), until
        /// no violation remains.
        std::vector<RawOptionQuote> remove_butterfly_arbitrage(const std::vector<RawOptionQuote> &quotes)
        {
            std::vector<RawOptionQuote> puts;
            std::map<Real, std::vector<RawOptionQuote>> by_expiry;
            for (const auto &q : quotes)
            {
                if (q.is_call)
                    by_expiry[q.ttm].push_back(q);
                else
                    puts.push_back(q);
            }

            std::vector<RawOptionQuote> clean_calls;
            for (auto &[ttm, group] : by_expiry)
            {
                std::sort(group.begin(), group.end(),
                          [](const RawOptionQuote &a, const RawOptionQuote &b)
                          { return a.strike < b.strike; });

                bool changed = true;
                while (changed && group.size() >= 3)
                {
                    changed = false;
                    std::vector<bool> keep(group.size(), true);
                    for (std::size_t i = 1; i + 1 < group.size(); ++i)
                    {
                        if (!(keep[i - 1] && keep[i] && keep[i + 1]))
                            continue;
                        const Real c0 = group[i - 1].mid();
                        const Real c1 = group[i].mid();
                        const Real c2 = group[i + 1].mid();
                        if (c0 - 2.0 * c1 + c2 < 0.0)
                        {
                            keep[i] = false;
                            changed = true;
                        }
                    }
                    std::vector<RawOptionQuote> survivors;
                    for (std::size_t i = 0; i < group.size(); ++i)
                        if (keep[i])
                            survivors.push_back(group[i]);
                    group = std::move(survivors);
                }
                clean_calls.insert(clean_calls.end(), group.begin(), group.end());
            }

            clean_calls.insert(clean_calls.end(), puts.begin(), puts.end());
            return clean_calls;
        }

    } // namespace

    RawVolSurface::RawVolSurface(std::vector<RawOptionQuote> raw_quotes,
                                 Real spot, Real rate, Real dividend,
                                 const CleaningParams &params)
        : spot_(spot), rate_(rate), dividend_(dividend)
    {
        stats_.raw_count = raw_quotes.size();

        const std::size_t oi_populated = static_cast<std::size_t>(
            std::count_if(raw_quotes.begin(), raw_quotes.end(),
                          [](const RawOptionQuote &q)
                          { return q.open_interest > 0; }));
        const bool oi_data_available =
            static_cast<Real>(oi_populated) > static_cast<Real>(stats_.raw_count) * params.oi_coverage_threshold;

        std::vector<RawOptionQuote> liquid;
        for (const auto &q : raw_quotes)
            if (passes_liquidity(q, params, oi_data_available))
                liquid.push_back(q);
        stats_.after_liquidity = liquid.size();

        std::vector<RawOptionQuote> moneyness_ok;
        for (const auto &q : liquid)
            if (passes_moneyness(q, spot_, params.min_moneyness, params.max_moneyness))
                moneyness_ok.push_back(q);
        stats_.after_moneyness = moneyness_ok.size();

        std::vector<RawOptionQuote> no_calendar_arb = remove_calendar_arbitrage(std::move(moneyness_ok));
        stats_.after_calendar_arbitrage = no_calendar_arb.size();

        std::vector<RawOptionQuote> no_butterfly_arb = remove_butterfly_arbitrage(no_calendar_arb);
        stats_.after_butterfly_arbitrage = no_butterfly_arb.size();

        quotes_ = std::move(no_butterfly_arb);
        stats_.final_count = quotes_.size();
    }

    Real RawVolSurface::forward(Real ttm) const noexcept
    {
        return spot_ * std::exp((rate_ - dividend_) * ttm);
    }

    Real RawVolSurface::log_moneyness(Real strike, Real ttm) const noexcept
    {
        return std::log(strike / forward(ttm));
    }

} // namespace quantModeling
