#include "quantModeling/engines/tree/trinomial.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace quantModeling
{
    TrinomialVanillaEngine::TrinomialVanillaEngine(PricingContext ctx)
        : EngineBase(std::move(ctx)), steps_(ctx_.settings.tree_steps)
    {
        if (steps_ < 1)
            throw InvalidInput("Trinomial tree requires steps >= 1");
    }

    void TrinomialVanillaEngine::visit(const VanillaOption &opt)
    {
        validate(opt);
        const auto &m = require_model<ILocalVolModel>("TrinomialVanillaEngine");

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();
        const Real T = opt.exercise->dates().front();
        // const Real K = opt.payoff->strike();      // Unused (kept for reference)
        // const OptionType type = opt.payoff->type();  // Unused (kept for reference)
        const bool is_american = opt.exercise->type() == ExerciseType::American;

        // Time step
        const Real dt = T / steps_;

        // Trinomial tree parameters (Boyle's approach)
        const Real u = std::exp(sigma * std::sqrt(3.0 * dt));
        // const Real d = 1.0 / u;  // Unused (kept for reference)

        // Drift and probabilities
        const Real nu = r - q - 0.5 * sigma * sigma;
        const Real dx = sigma * std::sqrt(3.0 * dt);

        const Real pu = 0.5 * ((sigma * sigma * dt + nu * nu * dt * dt) / (dx * dx) + nu * dt / dx);
        const Real pd = 0.5 * ((sigma * sigma * dt + nu * nu * dt * dt) / (dx * dx) - nu * dt / dx);
        const Real pm = 1.0 - pu - pd;

        const Real df = std::exp(-r * dt);

        if (!(pu >= 0.0 && pu <= 1.0 && pd >= 0.0 && pd <= 1.0 && pm >= 0.0 && pm <= 1.0))
            throw InvalidInput("Risk-neutral probabilities out of bounds. Check model parameters or reduce time step.");

        // Tree values at maturity
        const int max_nodes = 2 * steps_ + 1;
        std::vector<Real> values(max_nodes);

        // Initialize terminal values (at maturity, step N)
        for (int j = -steps_; j <= steps_; ++j)
        {
            const Real ST = S0 * std::pow(u, j);
            values[j + steps_] = (*opt.payoff)(ST);
        }

        // Backward induction through the tree
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = -i; j <= i; ++j)
            {
                const Real S = S0 * std::pow(u, j);

                // Continuation value (risk-neutral expectation)
                const int idx = j + steps_;
                const Real continuation = df * (pu * values[idx + 1] + pm * values[idx] + pd * values[idx - 1]);

                // For American: check early exercise, for European: use continuation
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values[idx] = std::max(continuation, exercise);
                }
                else
                {
                    values[idx] = continuation;
                }
            }
        }

        PricingResult out;
        out.npv = opt.notional * values[steps_];

        const char *exercise_label = is_american ? "American" : "European";
        out.diagnostics = std::string("Trinomial tree (Boyle) ") + exercise_label +
                          " vanilla (steps=" + std::to_string(steps_) + ")";

        // Greeks via finite differences
        const Real dS = S0 * 0.01;

        // Recompute price at S0 + dS
        std::vector<Real> values_up(max_nodes);
        for (int j = -steps_; j <= steps_; ++j)
        {
            const Real ST = (S0 + dS) * std::pow(u, j);
            values_up[j + steps_] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = -i; j <= i; ++j)
            {
                const Real S = (S0 + dS) * std::pow(u, j);
                const int idx = j + steps_;
                const Real continuation = df * (pu * values_up[idx + 1] + pm * values_up[idx] + pd * values_up[idx - 1]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_up[idx] = std::max(continuation, exercise);
                }
                else
                {
                    values_up[idx] = continuation;
                }
            }
        }

        // Recompute price at S0 - dS
        std::vector<Real> values_down(max_nodes);
        for (int j = -steps_; j <= steps_; ++j)
        {
            const Real ST = (S0 - dS) * std::pow(u, j);
            values_down[j + steps_] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = -i; j <= i; ++j)
            {
                const Real S = (S0 - dS) * std::pow(u, j);
                const int idx = j + steps_;
                const Real continuation = df * (pu * values_down[idx + 1] + pm * values_down[idx] + pd * values_down[idx - 1]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_down[idx] = std::max(continuation, exercise);
                }
                else
                {
                    values_down[idx] = continuation;
                }
            }
        }

        // Delta and Gamma
        out.greeks.delta = opt.notional * (values_up[steps_] - values_down[steps_]) / (2.0 * dS);
        out.greeks.gamma = opt.notional * (values_up[steps_] - 2.0 * values[steps_] + values_down[steps_]) / (dS * dS);

        // Vega: bump volatility
        const Real dsigma = 0.01;
        const Real sigma_bump = sigma + dsigma;
        const Real u_vega = std::exp(sigma_bump * std::sqrt(3.0 * dt));
        // const Real d_vega = 1.0 / u_vega;  // Unused (kept for reference)
        const Real dx_vega = sigma_bump * std::sqrt(3.0 * dt);
        const Real pu_vega = 0.5 * ((sigma_bump * sigma_bump * dt + nu * nu * dt * dt) / (dx_vega * dx_vega) + nu * dt / dx_vega);
        const Real pd_vega = 0.5 * ((sigma_bump * sigma_bump * dt + nu * nu * dt * dt) / (dx_vega * dx_vega) - nu * dt / dx_vega);
        const Real pm_vega = 1.0 - pu_vega - pd_vega;

        std::vector<Real> values_vega(max_nodes);
        for (int j = -steps_; j <= steps_; ++j)
        {
            const Real ST = S0 * std::pow(u_vega, j);
            values_vega[j + steps_] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = -i; j <= i; ++j)
            {
                const Real S = S0 * std::pow(u_vega, j);
                const int idx = j + steps_;
                const Real continuation = df * (pu_vega * values_vega[idx + 1] + pm_vega * values_vega[idx] + pd_vega * values_vega[idx - 1]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_vega[idx] = std::max(continuation, exercise);
                }
                else
                {
                    values_vega[idx] = continuation;
                }
            }
        }
        out.greeks.vega = opt.notional * (values_vega[steps_] - values[steps_]) / dsigma;

        // Theta: approximate with one less time step
        if (steps_ > 1)
        {
            const Real T_minus_dt = T - dt;
            const int steps_minus_1 = steps_ - 1;
            const Real dt_theta = T_minus_dt / steps_minus_1;
            const Real u_theta = std::exp(sigma * std::sqrt(3.0 * dt_theta));
            // const Real d_theta = 1.0 / u_theta;  // Unused (kept for reference)
            const Real dx_theta = sigma * std::sqrt(3.0 * dt_theta);
            const Real pu_theta = 0.5 * ((sigma * sigma * dt_theta + nu * nu * dt_theta * dt_theta) / (dx_theta * dx_theta) + nu * dt_theta / dx_theta);
            const Real pd_theta = 0.5 * ((sigma * sigma * dt_theta + nu * nu * dt_theta * dt_theta) / (dx_theta * dx_theta) - nu * dt_theta / dx_theta);
            const Real pm_theta = 1.0 - pu_theta - pd_theta;
            const Real df_theta = std::exp(-r * dt_theta);

            const int max_nodes_theta = 2 * steps_minus_1 + 1;
            std::vector<Real> values_theta(max_nodes_theta);
            for (int j = -steps_minus_1; j <= steps_minus_1; ++j)
            {
                const Real ST = S0 * std::pow(u_theta, j);
                values_theta[j + steps_minus_1] = (*opt.payoff)(ST);
            }
            for (int i = steps_minus_1 - 1; i >= 0; --i)
            {
                for (int j = -i; j <= i; ++j)
                {
                    const Real S = S0 * std::pow(u_theta, j);
                    const int idx = j + steps_minus_1;
                    const Real continuation = df_theta * (pu_theta * values_theta[idx + 1] + pm_theta * values_theta[idx] + pd_theta * values_theta[idx - 1]);
                    if (is_american)
                    {
                        const Real exercise = (*opt.payoff)(S);
                        values_theta[idx] = std::max(continuation, exercise);
                    }
                    else
                    {
                        values_theta[idx] = continuation;
                    }
                }
            }
            out.greeks.theta = -opt.notional * (values[steps_] - values_theta[steps_minus_1]) / dt;
        }

        res_ = out;
    }

    void TrinomialVanillaEngine::visit(const AsianOption &)
    {
        throw UnsupportedInstrument("TrinomialVanillaEngine does not support Asian options.");
    }

    void TrinomialVanillaEngine::visit(const EquityFuture &)
    {
        throw UnsupportedInstrument("TrinomialVanillaEngine does not support equity futures.");
    }

    void TrinomialVanillaEngine::visit(const ZeroCouponBond &)
    {
        throw UnsupportedInstrument("TrinomialVanillaEngine does not support bonds.");
    }

    void TrinomialVanillaEngine::visit(const FixedRateBond &)
    {
        throw UnsupportedInstrument("TrinomialVanillaEngine does not support bonds.");
    }

    void TrinomialVanillaEngine::validate(const VanillaOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("VanillaOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("VanillaOption.exercise is null");
        if (opt.exercise->dates().size() != 1)
        {
            throw InvalidInput("VanillaExercise must contain exactly one date (maturity)");
        }
        const Real T = opt.exercise->dates().front();
        if (!(T > 0.0))
            throw InvalidInput("Maturity T must be > 0");
        if (!(opt.notional > 0.0))
            throw InvalidInput("Notional must be > 0");
        const Real K = opt.payoff->strike();
        if (!(K > 0.0))
            throw InvalidInput("Strike must be > 0");
    }
} // namespace quantModeling
