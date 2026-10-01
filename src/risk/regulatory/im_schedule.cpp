#include "quantModeling/risk/regulatory/im_schedule.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling::im_schedule
{

    Real margin_rate(AssetClass asset_class, Time duration)
    {
        if (!(duration >= 0.0))
            throw InvalidInput("IM schedule: duration must be >= 0");
        const std::size_t bucket = duration <= 2.0 ? 0 : (duration <= 5.0 ? 1 : 2);
        switch (asset_class)
        {
            case AssetClass::Credit:
            {
                constexpr Real rates[] = {0.02, 0.05, 0.10};
                return rates[bucket];
            }
            case AssetClass::InterestRate:
            {
                constexpr Real rates[] = {0.01, 0.02, 0.04};
                return rates[bucket];
            }
            case AssetClass::ForeignExchange:
                return 0.06;
            case AssetClass::Commodity:
            case AssetClass::Equity:
            case AssetClass::Other:
                return 0.15;
        }
        throw InvalidInput("IM schedule: unknown asset class");
    }

    Real gross_initial_margin(const std::vector<Trade> &trades)
    {
        Real gross = 0.0;
        for (const Trade &trade : trades)
        {
            if (!(trade.notional >= 0.0) || !std::isfinite(trade.notional))
                throw InvalidInput("IM schedule: notional must be finite and >= 0");
            gross += trade.notional * margin_rate(trade.asset_class, trade.duration);
        }
        return gross;
    }

    Real net_to_gross_ratio(const std::vector<Trade> &trades)
    {
        Real net = 0.0;
        Real gross = 0.0;
        for (const Trade &trade : trades)
        {
            net += trade.market_value;
            gross += std::max(trade.market_value, 0.0);
        }
        return gross > 0.0 ? std::max(net, 0.0) / gross : 1.0;
    }

    Real net_initial_margin(Real gross_initial_margin, Real net_to_gross_ratio)
    {
        if (!(gross_initial_margin >= 0.0) ||
            !(net_to_gross_ratio >= 0.0 && net_to_gross_ratio <= 1.0))
            throw InvalidInput("IM schedule: gross IM must be >= 0 and NGR in [0, 1]");
        return (0.4 + 0.6 * net_to_gross_ratio) * gross_initial_margin;
    }

    Real net_initial_margin(const std::vector<Trade> &trades)
    {
        return net_initial_margin(gross_initial_margin(trades), net_to_gross_ratio(trades));
    }

} // namespace quantModeling::im_schedule
