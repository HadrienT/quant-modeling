#include "quantModeling/pricers/script_builders.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
        // 17 significant digits read back to the bit; the script lexer reads
        // no exponent, so a number %.17g would write with one is written in
        // plain decimals instead (enough of them for its 17 digits).
        char buf[400];
        std::snprintf(buf, sizeof buf, "%.17g", x);
        if (std::strchr(buf, 'e') == nullptr && std::strchr(buf, 'E') == nullptr)
            return buf;
        const int exponent = static_cast<int>(std::floor(std::log10(std::fabs(x))));
        const int decimals = std::max(0, 16 - exponent);
        std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
        std::string s = buf;
        if (s.find('.') != std::string::npos)
        {
            while (s.back() == '0')
                s.pop_back();
            if (s.back() == '.')
                s.pop_back();
        }
        return s;
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

    namespace
    {
        /// min(a, min(b, ...)) or max(...) of terms, two at a time.
        std::string fold(const char *f, const std::vector<std::string> &terms)
        {
            std::string out = terms.back();
            for (std::size_t i = terms.size() - 1; i-- > 0;)
                out = std::string(f) + "(" + terms[i] + ", " + out + ")";
            return out;
        }
    } // namespace

    std::string rainbow(bool worst, bool is_call, Real strike, Real notional, const std::vector<Real> &s0, int days)
    {
        std::vector<std::string> perfs;
        for (std::size_t i = 0; i < s0.size(); ++i)
            perfs.push_back("spot(" + std::to_string(i) + ") / " + number(s0[i]));
        const std::string perf = fold(worst ? "min" : "max", perfs);
        const std::string k = number(strike);
        const std::string payoff = is_call ? "max(p - " + k + ", 0)" : "max(" + k + " - p, 0)";
        return iso(days) + "\n    p = " + perf + "\n    pays " + number(notional) + " * " + payoff + "\n";
    }

    std::string basket(bool is_call, Real strike, const std::vector<Real> &weights, int days)
    {
        std::string b;
        for (std::size_t i = 0; i < weights.size(); ++i)
            b += (i ? " + " : "") + number(weights[i]) + " * spot(" + std::to_string(i) + ")";
        const std::string k = number(strike);
        return iso(days) + "\n    b = " + b + "\n    pays " + (is_call ? "max(b - " + k + ", 0)" : "max(" + k + " - b, 0)") +
               "\n";
    }

    std::string mountain(bool is_call, Real strike, Real notional, const std::vector<Real> &s0,
                         const std::vector<int> &dates)
    {
        const std::size_t n = s0.size();
        // One event per date: the returns, the best alive one, the lock.
        auto step = [&](bool first)
        {
            std::string s;
            if (first)
            {
                s += "    total = 0\n";
                for (std::size_t j = 0; j < n; ++j)
                    s += "    a" + std::to_string(j) + " = 1\n";
            }
            s += "    best = -2\n    k = -1\n"; // a return is at least -1
            for (std::size_t j = 0; j < n; ++j)
            {
                const std::string J = std::to_string(j);
                s += "    r" + J + " = spot(" + J + ") / " + number(s0[j]) + " - 1\n";
                s += "    if a" + J + " = 1 and r" + J + " > best then\n        best = r" + J + "\n        k = " + J +
                     "\n    endIf\n";
            }
            s += "    total = total + best\n";
            for (std::size_t j = 0; j < n; ++j)
                s += "    if k = " + std::to_string(j) + " then a" + std::to_string(j) + " = 0 endIf\n";
            return s;
        };
        const std::string k = number(strike);
        const std::string avg = "total / " + number(static_cast<double>(dates.size()));
        const std::string payoff =
            is_call ? "max(" + avg + " - " + k + ", 0)" : "max(" + k + " - " + avg + ", 0)";
        std::string script;
        for (std::size_t i = 0; i < dates.size(); ++i)
        {
            script += (i ? "\n" : "") + iso(dates[i]) + "\n" + step(i == 0);
            if (i + 1 == dates.size())
                script += "    pays " + number(notional) + " * " + payoff + "\n";
        }
        return script;
    }

    std::string autocall(const AutocallTerms &t, const std::vector<int> &dates)
    {
        const std::string N = number(t.notional), c = number(t.coupon_rate);
        const std::string ac = number(t.autocall_barrier * t.s0), cpn = number(t.coupon_barrier * t.s0),
                          put = number(t.put_barrier * t.s0), s0 = number(t.s0);
        const std::string periods = t.memory_coupon ? "(miss + 1)" : "1";
        std::string obs = "    if alive = 1 then\n";
        if (t.ki_on_every_date)
            obs += "        if spot() < " + put + " then ki = 1 endIf\n";
        obs += "        if spot() >= " + ac + " then\n"
                                              "            pays " +
               N + " * (1 + " + c + " * " + periods + ")\n"
                                                      "            alive = 0\n"
                                                      "        else\n"
                                                      "            if spot() >= " +
               cpn + " then\n"
                     "                pays " +
               N + " * " + c + " * " + periods + "\n"
                                                 "                miss = 0\n"
                                                 "            else\n"
                                                 "                miss = miss + 1\n"
                                                 "            endIf\n"
                                                 "        endIf\n"
                                                 "    endIf\n";
        const std::string init = "    alive = 1\n    miss = 0\n    ki = 0\n";
        std::string last = "    if alive = 1 then\n";
        if (!t.ki_on_every_date)
            last += "        if spot() < " + put + " then ki = 1 endIf\n";
        last += "        if ki = 1 then\n            pays " + N + " * spot() / " + s0 +
                "\n        else\n            pays " + N + "\n        endIf\n    endIf\n";
        if (dates.size() == 1)
            return iso(dates.front()) + "\n" + init + obs + last;
        return three_events(dates, init + obs, obs, last);
    }

    namespace
    {
        /// The observation dates, each running `step` (the first `init` +
        /// `step`), and `pay` at `pay_day`: on the last date when it is
        /// that day, in an event of its own when later.
        std::string observed(const std::vector<int> &dates, const std::string &init, const std::string &step,
                             const std::string &pay, int pay_day)
        {
            const bool same = dates.back() == pay_day;
            std::string s;
            if (dates.size() == 1)
                s = iso(dates.front()) + "\n" + init + step + (same ? pay : "");
            else
                s = three_events(dates, init + step, step, same ? pay : "");
            if (!same)
                s += "\n" + iso(pay_day) + "\n" + pay;
            return s;
        }
    } // namespace

    std::string realised_swap(bool volatility, Real s0, Real strike, Real notional, Real T,
                              const std::vector<int> &dates, int pay_day)
    {
        const std::string init = "    prev = " + number(s0) + "\n    acc = 0\n";
        const std::string step = "    l = log(spot() / prev)\n    acc = acc + l * l\n    prev = spot()\n";
        const std::string var = "acc / " + number(T);
        const std::string measure = volatility ? "sqrt(" + var + ")" : var;
        const std::string pay = "    pays " + number(notional) + " * (" + measure + " - " + number(strike) + ")\n";
        return observed(dates, init, step, pay, pay_day);
    }

    std::string dispersion(const std::vector<Real> &s0, const std::vector<Real> &weights, Real strike_spread,
                           Real notional, Real T, const std::vector<int> &dates, int pay_day)
    {
        std::string init, step, index, idx0, weighted;
        for (std::size_t i = 0; i < s0.size(); ++i)
        {
            const std::string I = std::to_string(i), w = number(weights[i]);
            init += "    p" + I + " = " + number(s0[i]) + "\n    a" + I + " = 0\n";
            step += "    l = log(spot(" + I + ") / p" + I + ")\n    a" + I + " = a" + I + " + l * l\n    p" + I +
                    " = spot(" + I + ")\n";
            index += (i ? " + " : "") + w + " * spot(" + I + ")";
            idx0 += (i ? " + " : "") + w + " * " + number(s0[i]);
            weighted += (i ? " + " : "") + w + " * a" + I;
        }
        init += "    pidx = " + idx0 + "\n    aidx = 0\n";
        step += "    nidx = " + index + "\n    l = log(nidx / pidx)\n    aidx = aidx + l * l\n    pidx = nidx\n";
        const std::string t = number(T);
        const std::string pay = "    pays " + number(notional) + " * ((" + weighted + ") / " + t + " - aidx / " + t +
                                " - " + number(strike_spread) + ")\n";
        return observed(dates, init, step, pay, pay_day);
    }

} // namespace quantModeling::script_builders
