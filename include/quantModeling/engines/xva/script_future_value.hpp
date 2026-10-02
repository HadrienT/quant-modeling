#ifndef QM_ENGINES_XVA_SCRIPT_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_SCRIPT_FUTURE_VALUE_HPP

#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/engines/xva/regression_future_value.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace quantModeling
{

    /**
     * @file script_future_value.hpp
     * @brief The exposure of a trade written as a payoff script
     *        (blueprint/wp/23-xva.md §13.1, §13.4, lot X4b): Andreasen &
     *        Savine's view of xVA, where any product the language can
     *        describe has an exposure, by regression of what it pays.
     *
     * The script runs on the paths of the exposure engine: at each event the
     * model state gives every discount factor the script asks for,
     * df(T) = P(t, T | x(t)). Replayed with a numeraire of one, its `pays`
     * are the cash flows of the path, date by date. Their value at each grid
     * date is the regression of the flows still to come (American
     * Monte-Carlo, regression_future_value.hpp) on the state **and on the
     * script's own variables**, which carry what the path has already
     * decided:
     *  - a variable that takes many values is a regressor — a coupon fixed
     *    and not yet paid;
     *  - a variable that takes a few values (four at most on the pilot
     *    paths) is a **regime**, with its own regression — a flag the script
     *    sets when it exercises or knocks out.
     *
     * A script with exercise() or call() runs under the rule fitted by
     * Longstaff-Schwartz (engines/mc/lsm.hpp), as for its price. Whether a
     * path has exercised is not visible from outside the script: **the
     * script must record it in a variable** (`done = 1`) for its exposure
     * after exercise to be right.
     *
     * Rates only: the engine simulates the Hull-White state, so a script
     * that reads spot() or a forward is refused.
     */

    struct ScriptExposureSettings
    {
        /// The regression of the value on the state and the variables.
        AmcSettings regression;
        /// Paths of the script's own pricing, which gives its value today
        /// and, for a script with exercise(), fits the exercise rule.
        int pricing_paths = 50000;
        std::uint64_t seed = 42;
    };

    /**
     * @param script the payoff script; its events must be after the
     *               valuation date of `ctx`.
     * @param model  must outlive the returned object.
     * @throws scripting::ScriptError on a script that does not parse;
     *         InvalidInput on one that reads spot() or a forward, has no
     *         future event, or has more variables than a regression can take.
     */
    std::unique_ptr<FutureValue> make_script_future_value(const std::string &script,
                                                          const ValuationContext &ctx,
                                                          const HullWhiteCurveModel &model,
                                                          const ScriptExposureSettings &settings = {});

} // namespace quantModeling

#endif // QM_ENGINES_XVA_SCRIPT_FUTURE_VALUE_HPP
