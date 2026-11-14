#ifndef QM_BENCHMARKS_REFERENCE_SCRIPTS_HPP
#define QM_BENCHMARKS_REFERENCE_SCRIPTS_HPP

// The worst-of autocall of reference_paths.hpp as a script and its
// three-asset model, for the script benchmarks (CPU and GPU).

#include "quantModeling/core/date.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <cmath>
#include <string>

namespace qm_reference
{

    /// Eight quarterly observations over two years from `valuation` (the
    /// reference's equal dates, to the day).
    inline std::string worst_of_autocall_script(const quantModeling::Date &valuation)
    {
        const std::string worst = "    worst = min(spot(0), min(spot(1), spot(2))) / 100\n";
        std::string s;
        for (int k = 1; k <= 8; ++k)
        {
            s += (valuation + static_cast<int>(std::lround(730.0 * k / 8.0))).to_iso() + "\n";
            if (k == 1)
                s += "    alive = 1\n";
            s += worst + "    if alive = 1 then\n";
            if (k < 8)
                s += "        if worst >= 1 then\n            pays 1025\n            alive = 0\n        else\n"
                     "            if worst >= 0.7 then pays 25 endIf\n        endIf\n";
            else
                s += "        if worst >= 0.7 then\n            pays 1025\n        else\n"
                     "            if worst < 0.6 then pays 1000 * worst else pays 1000 endIf\n        endIf\n";
            s += "    endIf\n\n";
        }
        return s;
    }

    /// Three assets at 100, vol 25 %, pairwise correlation 0.4.
    inline quantModeling::scripting::ScriptModelSpec worst_of_autocall_model(double rate)
    {
        quantModeling::scripting::ScriptModelSpec s;
        s.rate = rate;
        s.spots = {100.0, 100.0, 100.0};
        s.dividends = {0.0, 0.0, 0.0};
        s.vols = {0.25, 0.25, 0.25};
        s.correlation = {1.0, 0.4, 0.4, 0.4, 1.0, 0.4, 0.4, 0.4, 1.0};
        return s;
    }

} // namespace qm_reference

#endif // QM_BENCHMARKS_REFERENCE_SCRIPTS_HPP
