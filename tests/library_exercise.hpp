#ifndef QM_TESTS_LIBRARY_EXERCISE_HPP
#define QM_TESTS_LIBRARY_EXERCISE_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"

#include <string>

namespace quantModeling
{
    /// Whether a library script decides with exercise() / call(). Such a
    /// script prices only through fit_exercise_policy (engines/mc/lsm.hpp):
    /// the loops over the whole library, which run the engines with no
    /// fitted rule, leave it to testScriptIrExercise and the Lsm tests.
    inline bool library_script_has_exercise(const std::string &source)
    {
        static const ValuationContext ctx{Date::from_iso("2026-06-01")};
        return ScriptedProduct<Real>(source, ctx).has_exercise();
    }
} // namespace quantModeling

#endif
