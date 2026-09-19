#ifndef QM_SCRIPTING_MODEL_ADVICE_HPP
#define QM_SCRIPTING_MODEL_ADVICE_HPP

#include "quantModeling/scripting/visitors/script_analyzer.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace quantModeling::scripting
{

    enum class ModelKind
    {
        BlackScholesFlatVol, ///< one constant vol: no smile, no skew dynamics
        LocalVolSurface      ///< Dupire surface: today's vanilla smile, exact
    };

    struct Advice
    {
        std::string code;
        std::string severity; ///< "warning" | "info"
        std::string message;
    };

    /**
     * @brief What to tell the user when the model they chose cannot capture
     *        what the script's price depends on.
     *
     * Only structural mismatches are reported (see ScriptAnalysis): never a
     * guess about how large the pricing error is. `surface_max_T` is the
     * longest maturity a calibrated surface covers (ignored, and pass 0,
     * for a flat-vol model); `event_horizon` is the script's last event.
     */
    inline std::vector<Advice> advise(const ScriptAnalysis &a, ModelKind model,
                                      double event_horizon, double surface_max_T)
    {
        std::vector<Advice> out;

        if (model == ModelKind::BlackScholesFlatVol &&
            (a.nonlinear_in_spot || a.spot_threshold_test))
        {
            out.push_back(
                {"flat_vol_smile", "warning",
                 a.spot_threshold_test
                     ? "The script tests spot against a level (a barrier, "
                       "digital or trigger shape) but the model has one flat "
                       "volatility: these payoffs are the most sensitive to "
                       "skew, which a flat vol cannot represent. Price with "
                       "model='local_vol' and a ticker to use the market "
                       "surface."
                     : "The payoff is nonlinear in spot, so its price depends "
                       "on the smile at the relevant strikes, but the model "
                       "has one flat volatility, which matches the market at "
                       "one strike and maturity only. Price with "
                       "model='local_vol' and a ticker to use the market "
                       "surface."});
        }

        if (a.path_dependent)
        {
            out.push_back(
                {"forward_smile", "info",
                 model == ModelKind::LocalVolSurface
                     ? "The payoff depends on spot at several dates jointly "
                       "(a running quantity or a trigger carried across "
                       "events). Local vol reproduces today's vanilla smile "
                       "exactly, but the forward smile it implies is flatter "
                       "than what markets typically show: treat forward-skew "
                       "risk as approximate. A stochastic-vol model would be "
                       "needed for it, and is not yet available for scripts."
                     : "The payoff depends on spot at several dates jointly "
                       "(a running quantity or a trigger carried across "
                       "events), so it depends on the forward smile, which "
                       "a flat-vol model does not have."});
        }

        if (model == ModelKind::LocalVolSurface && surface_max_T > 0.0 &&
            event_horizon > surface_max_T + 1e-9)
        {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "The script has events out to T=%.2f years but the "
                          "calibrated surface stops at T=%.2f: local vol is "
                          "held flat beyond it (extrapolated, not observed).",
                          event_horizon, surface_max_T);
            out.push_back({"surface_extrapolated", "warning", buf});
        }

        return out;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_MODEL_ADVICE_HPP
