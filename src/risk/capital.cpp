#include "quantModeling/risk/capital.hpp"

#include "quantModeling/risk/regulatory/irb.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;

        /// How long the trade's underlying still runs.
        Time remaining(const sa_ccr::Trade &trade)
        {
            return std::max(trade.maturity, trade.end);
        }

        struct DateCharges
        {
            Real add_on_unmargined = 0.0;
            Real add_on_margined = 0.0;
            /// Capital per unit of EAD.
            Real default_rate = 0.0;
            Real cva_rate = 0.0;
        };

        DateCharges charges(const CapitalInputs &in, Time t)
        {
            std::vector<sa_ccr::Trade> live;
            for (const sa_ccr::Trade &trade : in.trades)
                if (const auto aged = aged_trade(trade, t))
                    live.push_back(*aged);
            DateCharges c;
            if (live.empty())
                return c;
            c.add_on_unmargined = sa_ccr::add_ons(live).aggregate();
            if (in.margin)
                c.add_on_margined = sa_ccr::add_ons(live, in.margin).aggregate();
            const Time m = effective_maturity(in.trades, t);
            c.default_rate = irb::capital_requirement(std::max(in.pd, irb_pd_floor), in.lgd,
                                                      std::min(m, 5.0), in.large_financial);
            // One counterparty, one netting set: the reduced BA-CVA is linear
            // in the EAD.
            ba_cva::Counterparty counterparty;
            counterparty.sector = in.sector;
            counterparty.quality = in.quality;
            counterparty.netting_sets.push_back({1.0, m, false});
            c.cva_rate = ba_cva::capital_reduced({counterparty});
            return c;
        }
    } // namespace

    std::optional<sa_ccr::Trade> aged_trade(const sa_ccr::Trade &trade, Time t)
    {
        if (!(t >= 0.0))
            throw InvalidInput("capital projection: the date must be >= 0");
        if (trade.maturity - t <= kTimeEps)
            return std::nullopt;
        sa_ccr::Trade aged = trade;
        aged.maturity = trade.maturity - t;
        aged.start = std::max(trade.start - t, 0.0);
        aged.end = std::max(trade.end - t, 0.0);
        if (aged.option)
        {
            aged.option->exercise = trade.option->exercise - t;
            if (aged.option->exercise <= kTimeEps)
            {
                // Past its last exercise date: the underlying, held in the
                // direction the option gave (a bought call or a sold put
                // rises with the risk factor).
                const bool call = trade.option->type == sa_ccr::OptionType::Call;
                const bool bought = trade.option->side == sa_ccr::OptionSide::Bought;
                aged.long_primary_risk_factor = call == bought;
                aged.option.reset();
            }
        }
        return aged;
    }

    Time effective_maturity(const std::vector<sa_ccr::Trade> &trades, Time t)
    {
        Real weighted = 0.0, notional = 0.0;
        for (const sa_ccr::Trade &trade : trades)
        {
            const Time left = remaining(trade) - t;
            if (trade.maturity - t <= kTimeEps || left <= kTimeEps)
                continue;
            weighted += trade.notional * left;
            notional += trade.notional;
        }
        return notional > 0.0 ? std::max(weighted / notional, 1.0) : 0.0;
    }

    CapitalProfile projected_capital(const ExposurePaths &cube, const CapitalInputs &in,
                                     const InitialMargin *margin_received)
    {
        if (cube.trades() != 1 || cube.paths == 0 || cube.dates() == 0)
            throw InvalidInput("capital projection: needs the cube of one netting set (one trade)");
        if (cube.measure != ExposureMeasure::RiskNeutral)
            throw InvalidInput("capital projection: KVA is a price and needs a risk-neutral "
                               "simulation");
        if (!(in.pd >= 0.0 && in.pd < 1.0) || !(in.lgd >= 0.0 && in.lgd <= 1.0))
            throw InvalidInput("capital projection: need 0 <= PD < 1 and 0 <= LGD <= 1");
        const std::size_t n = cube.dates(), N = cube.paths;
        if (margin_received != nullptr &&
            (margin_received->paths != N || margin_received->times != cube.times))
            throw InvalidInput("capital projection: the initial margin is not that of this cube");

        // EAD on one path: the margin held lowers V - C and counts as NICA.
        const auto ead = [&in](Real value_minus_collateral, Real margin_held, const DateCharges &c)
        {
            if (!in.margin)
                return sa_ccr::exposure_at_default(value_minus_collateral - margin_held,
                                                   c.add_on_unmargined);
            sa_ccr::MarginAgreement agreement = *in.margin;
            agreement.nica += margin_held;
            return sa_ccr::exposure_at_default(value_minus_collateral - margin_held,
                                               c.add_on_unmargined, agreement, c.add_on_margined);
        };

        CapitalProfile out;
        out.times = cube.times;
        const DateCharges today = charges(in, 0.0);
        out.ead_today = ead(cube.trade_values_today[0],
                            margin_received != nullptr ? margin_received->today : 0.0, today);
        out.default_capital_today = today.default_rate * out.ead_today;
        out.cva_capital_today = today.cva_rate * out.ead_today;

        out.expected_ead.assign(n, 0.0);
        out.discounted_default_capital.assign(n, 0.0);
        out.discounted_cva_capital.assign(n, 0.0);
        out.discounted_capital.assign(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            const DateCharges c = charges(in, cube.times[i]);
            Real sum = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real held = margin_received != nullptr ? margin_received->margin[p * n + i] : 0.0;
                sum += cube.discount_weight[p * n + i] * ead(cube.trade_values[0][p * n + i], held, c);
            }
            const Real discounted_ead = sum / static_cast<Real>(N);
            out.expected_ead[i] = discounted_ead / cube.discount[i];
            out.discounted_default_capital[i] = c.default_rate * discounted_ead;
            out.discounted_cva_capital[i] = c.cva_rate * discounted_ead;
            out.discounted_capital[i] =
                out.discounted_default_capital[i] + out.discounted_cva_capital[i];
        }
        return out;
    }

} // namespace quantModeling
