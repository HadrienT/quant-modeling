#include "quantModeling/pricers/script_builders.hpp"

#include <algorithm>
#include <cmath>
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
        // The parser reads digits, a dot and an exponent; no "inf" or "nan"
        // ever comes from a product's terms.
        return buf;
    }

    std::vector<int> evenly(int days, int observations)
    {
        std::set<int> out;
        for (int k = 1; k <= observations; ++k)
            out.insert(std::max(
                1, static_cast<int>((static_cast<long long>(k) * days + observations / 2) / observations)));
        out.insert(days);
        return {out.begin(), out.end()};
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

        /// The first date, the dates in between (on one line), the last
        /// date, each with its statements; the last date runs `middle` then
        /// `last`.
        std::string three_events(const std::vector<int> &dates, const std::string &first, const std::string &middle,
                                 const std::string &last)
        {
            if (dates.size() == 1)
                return iso(dates.front()) + "\n" + first + last;
            std::string s = iso(dates.front()) + "\n" + first + "\n";
            if (dates.size() > 2)
            {
                for (std::size_t i = 1; i + 1 < dates.size(); ++i)
                    s += (i > 1 ? "  " : "") + iso(dates[i]);
                s += "\n" + middle + "\n";
            }
            return s + iso(dates.back()) + "\n" + middle + last;
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
        if (dates.size() == 1)
            return iso(days) + "\n    s = 1\n" + check + payment("s");
        return three_events(dates, "    s = 1\n" + check, check, payment("s"));
    }

    std::string asian(const AsianTerms &t, const std::vector<int> &fixings)
    {
        const std::string n = number(static_cast<double>(fixings.size()));
        const std::string k = number(t.strike);
        const bool logs = t.geometric || t.geometric_control;
        const std::string first =
            std::string(t.geometric ? "" : "    acc = spot()\n") + (logs ? "    lacc = log(spot())\n" : "");
        const std::string middle =
            std::string(t.geometric ? "" : "    acc = acc + spot()\n") + (logs ? "    lacc = lacc + log(spot())\n" : "");
        const std::string avg = t.geometric ? "exp(lacc / " + n + ")" : "acc / " + n;
        auto payoff = [&](const std::string &a)
        { return t.is_call ? "max(" + a + " - " + k + ", 0)" : "max(" + k + " - " + a + ", 0)"; };
        std::string last = "    pays " + payoff(avg) + "\n";
        if (t.geometric_control && !t.geometric)
            last += "    controlPays " + payoff("exp(lacc / " + n + ")") + "\n";
        if (fixings.size() == 1)
            return iso(fixings.front()) + "\n" + first + last;
        return three_events(fixings, first, middle, last);
    }

    double geometric_asian_bs(double spot, double rate, double dividend, double vol, double strike, bool is_call,
                              const std::vector<double> &times)
    {
        const double n = static_cast<double>(times.size());
        double mean_t = 0.0, sum_min = 0.0;
        for (double ti : times)
            mean_t += ti / n;
        for (double ti : times)
            for (double tj : times)
                sum_min += std::min(ti, tj);
        const double mu = std::log(spot) + (rate - dividend - 0.5 * vol * vol) * mean_t;
        const double var = vol * vol * sum_min / (n * n);
        const double sd = std::sqrt(var);
        const double forward = std::exp(mu + 0.5 * var);
        const double df = std::exp(-rate * times.back());
        auto N = [](double x)
        { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
        if (!(sd > 0.0))
            return df * std::max(is_call ? forward - strike : strike - forward, 0.0);
        const double d1 = (mu - std::log(strike) + var) / sd;
        const double d2 = d1 - sd;
        return is_call ? df * (forward * N(d1) - strike * N(d2)) : df * (strike * N(-d2) - forward * N(-d1));
    }

    std::string lookback(const LookbackTerms &t, Real s0, const std::vector<int> &fixings)
    {
        const std::string s = number(s0), k = number(t.strike);
        const std::string first = "    hi = max(" + s + ", spot())\n    lo = min(" + s + ", spot())\n";
        const std::string middle = "    hi = max(hi, spot())\n    lo = min(lo, spot())\n";
        std::string payoff;
        if (t.floating)
            payoff = t.is_call ? "spot() - lo" : "hi - spot()";
        else
        {
            const std::string e = t.on_maximum ? "hi" : "lo";
            payoff = t.is_call ? "max(" + e + " - " + k + ", 0)" : "max(" + k + " - " + e + ", 0)";
        }
        if (fixings.size() == 1)
            return iso(fixings.front()) + "\n" + first + "    pays " + payoff + "\n";
        return three_events(fixings, first, middle, "    pays " + payoff + "\n");
    }

} // namespace quantModeling::script_builders
