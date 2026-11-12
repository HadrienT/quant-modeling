#include "quantModeling/pricers/script_builders.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace quantModeling::script_builders
{

    Date valuation()
    {
        return Date::from_iso("2026-01-01");
    }

    std::string number(double x)
    {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", x);
        std::string s = buf;
        // The parser reads digits, a dot and an exponent; no "inf" or "nan"
        // ever comes from a product's terms.
        return s;
    }

    namespace
    {
        std::string iso(int days)
        {
            return (valuation() + days).to_iso();
        }

        std::string vanilla(bool is_call, Real strike)
        {
            const std::string k = number(strike);
            return is_call ? "max(spot() - " + k + ", 0)" : "max(" + k + " - spot(), 0)";
        }

        /// `observations` dates evenly spread over (0, days], the last on
        /// `days`, one per calendar day at most.
        std::vector<int> evenly(int days, int observations)
        {
            std::set<int> out;
            for (int k = 1; k <= observations; ++k)
                out.insert(std::max(1, static_cast<int>((static_cast<long long>(k) * days + observations / 2) /
                                                        observations)));
            out.insert(days);
            return {out.begin(), out.end()};
        }
    } // namespace

    std::string barrier(const BarrierTerms &t, int days, int observations)
    {
        const bool up = t.type == BarrierType::UpAndOut || t.type == BarrierType::UpAndIn;
        const bool out = t.type == BarrierType::DownAndOut || t.type == BarrierType::UpAndOut;
        const std::string van = vanilla(t.is_call, t.strike);
        const std::string rebate = number(t.rebate);
        // The survival s weighs the vanilla (out) or the rebate (in).
        auto payment = [&](const std::string &s)
        {
            const std::string alive = out ? s : "(1 - " + s + ")";
            const std::string dead = out ? "(1 - " + s + ")" : s;
            std::string p = "    pays " + alive + " * " + van;
            if (t.rebate != 0.0)
                p += " + " + dead + " * " + rebate;
            return p + "\n";
        };

        if (observations <= 0)
        {
            const std::string level = number(t.level);
            return iso(days) + "\n    s = " + (up ? "survivalBelow(" : "survivalAbove(") + level + ")\n" +
                   payment("s");
        }

        // Discrete monitoring: s = 1 until a date's close is on or beyond
        // the barrier (the hand-written engine's check).
        const std::string hit = up ? "spot() >= " + number(t.level) : "spot() <= " + number(t.level);
        const std::string check = "    if " + hit + " then s = 0 endIf\n";
        const std::vector<int> dates = evenly(days, observations);
        std::string script;
        if (dates.size() == 1)
            return iso(days) + "\n    s = 1\n" + check + payment("s");
        script += iso(dates.front()) + "\n    s = 1\n" + check + "\n";
        if (dates.size() > 2)
        {
            for (std::size_t i = 1; i + 1 < dates.size(); ++i)
                script += (i > 1 ? "  " : "") + iso(dates[i]);
            script += "\n" + check + "\n";
        }
        script += iso(dates.back()) + "\n" + check + payment("s");
        return script;
    }

} // namespace quantModeling::script_builders
