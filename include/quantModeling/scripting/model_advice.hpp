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
        LocalVolSurface,     ///< Dupire surface: today's vanilla smile, exact
        Heston,              ///< calibrated stochastic vol: its own forward smile, today's smile up to the fit error
        StochasticLocalVol   ///< Heston dynamics x leverage: today's smile exact, Heston's forward smile
    };

    /// The name price_script and make_script_model use for each model.
    inline const char *model_name(ModelKind m) noexcept
    {
        switch (m)
        {
            case ModelKind::BlackScholesFlatVol:
                return "black_scholes";
            case ModelKind::LocalVolSurface:
                return "local_vol";
            case ModelKind::Heston:
                return "heston";
            case ModelKind::StochasticLocalVol:
                return "slv";
        }
        return "black_scholes";
    }

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
     * for a model without one); `event_horizon` is the script's last event.
     */
    inline std::vector<Advice> advise(const ScriptAnalysis &a, ModelKind model,
                                      double event_horizon, double surface_max_T)
    {
        std::vector<Advice> out;
        const bool smile_sensitive = a.nonlinear_in_spot || a.spot_threshold_test;

        const bool multi_asset = a.n_underlyings > 1;

        if (multi_asset)
        {
            if (smile_sensitive)
                out.push_back(
                    {"flat_vol_smile", "warning",
                     "The script reads several underlyings, each simulated "
                     "with one flat volatility: the skew of each asset is not "
                     "captured, and worst-of or barrier payoffs are the most "
                     "sensitive to it. Multi-asset local vol is not available "
                     "for scripts yet."});
            out.push_back(
                {"correlation", "info",
                 "The price depends on the joint law of the underlyings: the "
                 "correlation matrix is an input (historical, or typed), not "
                 "implied from market prices. Worst-of and best-of payoffs "
                 "are the most sensitive to it."});
        }
        else if (model == ModelKind::BlackScholesFlatVol && smile_sensitive)
        {
            out.push_back(
                {"flat_vol_smile", "warning",
                 a.spot_threshold_test
                     ? "The script tests spot against a level (a barrier, "
                       "digital or trigger shape) but the model has one flat "
                       "volatility: these payoffs are the most sensitive to "
                       "skew, which a flat vol cannot represent. Price with "
                       "model='auto' (or 'local_vol') and a ticker to use the "
                       "market surface."
                     : "The payoff is nonlinear in spot, so its price depends "
                       "on the smile at the relevant strikes, but the model "
                       "has one flat volatility, which matches the market at "
                       "one strike and maturity only. Price with "
                       "model='auto' (or 'local_vol') and a ticker to use the "
                       "market surface."});
        }

        if (model == ModelKind::Heston && smile_sensitive)
        {
            out.push_back(
                {"heston_fit", "info",
                 "Heston has five parameters for the whole surface, so it "
                 "reprices today's vanillas only up to its calibration error "
                 "(reported with the model). local_vol and slv reprice them "
                 "exactly."});
        }

        if (a.path_dependent)
        {
            const char *msg = nullptr;
            switch (model)
            {
                case ModelKind::LocalVolSurface:
                    msg = "The payoff depends on spot at several dates jointly "
                          "(a running quantity or a trigger carried across "
                          "events). Local vol reproduces today's vanilla smile "
                          "exactly, but the forward smile it implies is flatter "
                          "than what markets typically show: treat forward-skew "
                          "risk as approximate. model='slv' adds calibrated "
                          "stochastic-vol dynamics.";
                    break;
                case ModelKind::Heston:
                    msg = "The payoff depends on spot at several dates jointly, "
                          "so on the forward smile. Heston carries one, from its "
                          "calibrated vol of vol and correlation, but its "
                          "marginals match today's surface only up to the "
                          "calibration error; model='slv' keeps these dynamics "
                          "and makes the marginals exact.";
                    break;
                case ModelKind::StochasticLocalVol:
                    msg = "The payoff depends on spot at several dates jointly, "
                          "so on the forward smile. SLV reprices today's "
                          "vanillas and takes its forward smile from the "
                          "calibrated Heston dynamics at full vol of vol: no "
                          "mixing fraction is applied, since calibrating one "
                          "needs exotic quotes (barriers, forward-starts) that "
                          "are not stored. Forward-skew risk rests on that "
                          "choice.";
                    break;
                case ModelKind::BlackScholesFlatVol:
                    msg = "The payoff depends on spot at several dates jointly "
                          "(a running quantity or a trigger carried across "
                          "events), so it depends on the forward smile, which "
                          "a flat-vol model does not have.";
                    break;
            }
            out.push_back({"forward_smile", "info", msg});
        }

        const bool has_surface = model == ModelKind::LocalVolSurface ||
                                 model == ModelKind::StochasticLocalVol;
        if (has_surface && surface_max_T > 0.0 &&
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

    /// What the caller can calibrate for this pricing.
    struct ModelAvailability
    {
        bool market_surface = false; ///< a stored option chain -> SVI + Dupire (local_vol)
        bool stochastic = false;     ///< a Heston fit and an SLV leverage on that surface
    };

    struct Recommendation
    {
        ModelKind model = ModelKind::BlackScholesFlatVol;
        std::string code;   ///< stable key for the reason (UI, tests)
        std::string reason; ///< why this model, in one or two sentences
    };

    /**
     * @brief The simplest model that captures everything the script's price
     *        depends on, among those the caller can calibrate.
     *
     * The rule a desk applies, read off the script's structure: a payoff
     * that depends on each date's spot separately (vanillas, digitals, a sum
     * of Europeans) depends only on the marginals, which local vol
     * reproduces exactly from today's surface -- a stochastic model would
     * add cost, not accuracy. A payoff carrying state across dates (a
     * barrier, an average, a knock-in) depends on the forward smile, hence
     * SLV: Dupire-exact marginals plus stochastic-vol dynamics. A script
     * mixing both (a vanilla and a barrier) gets the one model its most
     * demanding part needs, since the parts cannot be priced under two laws
     * of spot and stay consistent; the calibrated model still reprices the
     * vanilla part.
     *
     * Heston alone is never recommended: it reprices today's vanillas only
     * up to its fit error, and SLV keeps its dynamics without that error.
     * It remains available to a caller who asks for it.
     */
    inline Recommendation recommend(const ScriptAnalysis &a, const ModelAvailability &avail)
    {
        if (a.n_underlyings > 1)
            return {ModelKind::BlackScholesFlatVol, "multi_asset",
                    "The script reads several underlyings, so they are "
                    "simulated jointly: correlated Black-Scholes, each asset at "
                    "its own volatility, with the correlation matrix given. It "
                    "is the only multi-asset model available for scripts."};

        if (!avail.market_surface)
            return {ModelKind::BlackScholesFlatVol, "no_market_data",
                    "No market surface to calibrate (no ticker with a stored "
                    "option chain): one flat volatility, as typed, is the "
                    "only model available."};

        if (a.path_dependent)
        {
            if (avail.stochastic)
                return {ModelKind::StochasticLocalVol, "path_dependent",
                        "The payoff carries state across dates (a running "
                        "quantity, a trigger or a barrier), so its price "
                        "depends on the forward smile. Stochastic-local vol: "
                        "Heston dynamics calibrated to the surface give the "
                        "forward smile, and the leverage function makes every "
                        "vanilla of today's surface reprice exactly."};
            return {ModelKind::LocalVolSurface, "stochastic_unavailable",
                    "The payoff carries state across dates, so it depends on "
                    "the forward smile, which stochastic-local vol would "
                    "capture; its calibration is not available for this "
                    "surface, so local vol is used: exact on today's "
                    "vanillas, flatter forward smile."};
        }

        if (a.nonlinear_in_spot || a.spot_threshold_test)
            return {ModelKind::LocalVolSurface, "terminal_smile",
                    "The payoff depends on spot at each date separately "
                    "(its marginal law), so today's surface prices it "
                    "exactly and local vol reproduces that surface. A "
                    "stochastic model would add cost, not accuracy."};

        return {ModelKind::LocalVolSurface, "linear_payoff",
                "The payoff is linear in spot: its price depends only on "
                "forwards, and every model calibrated to them agrees. Local "
                "vol on the market surface is used for its spot and "
                "dividend."};
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_MODEL_ADVICE_HPP
