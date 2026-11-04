#include "quantModeling/engines/xva/pilot_targets.hpp"

namespace quantModeling
{

    PilotTargets pilot_targets(const FutureValue &trade, const PilotPaths &pilot, bool differentials)
    {
        const std::size_t n = pilot.dates();
        const std::size_t N = pilot.paths;
        if (pilot.state.size() != N * n || pilot.deflator.size() != N * n)
            throw InvalidInput("pilot targets: the pilot's matrices do not match its paths and dates");
        if (differentials && !pilot.has_sensitivities())
            throw InvalidInput("pilot targets: the pilot paths carry no sensitivities of the state "
                               "and of the deflator, which the differentials need");

        PilotTargets out;
        out.paths = N;
        out.dates = n;
        out.value.resize(N * n);
        if (differentials)
            out.differential.resize(N * n);

        // g[k]: what the flows already gathered read directly from state_k.
        std::vector<Real> direct(differentials ? n : 0);
        std::vector<StateDerivative> gradient;
        for (std::size_t p = 0; p < N; ++p)
        {
            const Real *state = pilot.state_row(p);
            const Real *deflator = pilot.deflator_row(p);
            Real *value = out.value.data() + p * n;
            Real *differential = differentials ? out.differential.data() + p * n : nullptr;
            if (differentials)
                std::fill(direct.begin(), direct.end(), 0.0);
            Real future = 0.0;  // F_i
            Real carried = 0.0; // H_{i+1} + ∂(c_{i+1} D_{i+1}) / ∂state_{i+1}
            for (std::size_t i = n; i-- > 0;)
            {
                value[i] = future / deflator[i];
                Real H = 0.0;
                if (differentials)
                {
                    H = direct[i] + (i + 1 < n ? pilot.state_decay[i + 1] * carried : 0.0);
                    differential[i] = H / deflator[i] - pilot.deflator_slope[i] * value[i];
                }
                const Real flow = trade.cashflow(i, state) * deflator[i];
                future += flow;
                if (!differentials)
                    continue;
                gradient.clear();
                if (!trade.cashflow_gradient(i, state, gradient))
                    throw InvalidInput("pilot targets: the trade gives no gradient of its cash "
                                       "flows, which the differentials need");
                Real own = flow * pilot.deflator_slope[i];
                for (const StateDerivative &d : gradient)
                {
                    if (d.date > i)
                        throw InvalidInput("pilot targets: a cash flow cannot read a later state");
                    if (d.date == i)
                        own += d.value * deflator[i];
                    else
                        direct[d.date] += d.value * deflator[i];
                }
                carried = H + own;
            }
        }
        return out;
    }

} // namespace quantModeling
