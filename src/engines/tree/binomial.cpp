#include "quantModeling/engines/tree/binomial.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace quantModeling
{
    BinomialVanillaEngine::BinomialVanillaEngine(PricingContext ctx)
        : EngineBase(std::move(ctx)), steps_(ctx_.settings.tree_steps)
    {
        if (steps_ < 1)
            throw InvalidInput("Binomial tree requires steps >= 1");
    }

    void BinomialVanillaEngine::visit(const VanillaOption &opt)
    {
        validate(opt);
        const auto &m = require_model<ILocalVolModel>("BinomialVanillaEngine");

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

        // CRR binomial tree parameters
        const Real u = std::exp(sigma * std::sqrt(dt)); // up factor
        const Real d = 1.0 / u;                         // down factor
        const Real a = std::exp((r - q) * dt);          // drift factor
        const Real p = (a - d) / (u - d);               // risk-neutral probability
        const Real df = std::exp(-r * dt);              // discount factor

        if (!(p >= 0.0 && p <= 1.0))
            throw InvalidInput("Risk-neutral probability out of bounds [0,1]. Check model parameters.");

        // Tree values at maturity (step N)
        std::vector<Real> values(steps_ + 1);

        // Initialize terminal values (at maturity)
        for (int j = 0; j <= steps_; ++j)
        {
            const Real ST = S0 * std::pow(u, j) * std::pow(d, steps_ - j);
            values[j] = (*opt.payoff)(ST);
        }

        // Backward induction through the tree
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = 0; j <= i; ++j)
            {
                const Real S = S0 * std::pow(u, j) * std::pow(d, i - j);

                // Continuation value (risk-neutral expectation)
                const Real continuation = df * (p * values[j + 1] + (1.0 - p) * values[j]);

                // For American: check early exercise, for European: use continuation
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values[j] = std::max(continuation, exercise);
                }
                else
                {
                    values[j] = continuation;
                }
            }
        }

        PricingResult out;
        out.npv = opt.notional * values[0];

        const char *exercise_label = is_american ? "American" : "European";
        out.diagnostics = std::string("Binomial tree (CRR) ") + exercise_label +
                          " vanilla (steps=" + std::to_string(steps_) + ")";

        // Simple finite difference approximation for Greeks
        const Real dS = S0 * 0.01; // 1% bump

        // Recompute price at S0 + dS (up value)
        std::vector<Real> values_up(steps_ + 1);
        for (int j = 0; j <= steps_; ++j)
        {
            const Real ST = (S0 + dS) * std::pow(u, j) * std::pow(d, steps_ - j);
            values_up[j] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = 0; j <= i; ++j)
            {
                const Real S = (S0 + dS) * std::pow(u, j) * std::pow(d, i - j);
                const Real continuation = df * (p * values_up[j + 1] + (1.0 - p) * values_up[j]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_up[j] = std::max(continuation, exercise);
                }
                else
                {
                    values_up[j] = continuation;
                }
            }
        }

        // Recompute price at S0 - dS (down value)
        std::vector<Real> values_down(steps_ + 1);
        for (int j = 0; j <= steps_; ++j)
        {
            const Real ST = (S0 - dS) * std::pow(u, j) * std::pow(d, steps_ - j);
            values_down[j] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = 0; j <= i; ++j)
            {
                const Real S = (S0 - dS) * std::pow(u, j) * std::pow(d, i - j);
                const Real continuation = df * (p * values_down[j + 1] + (1.0 - p) * values_down[j]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_down[j] = std::max(continuation, exercise);
                }
                else
                {
                    values_down[j] = continuation;
                }
            }
        }

        // Centered finite difference
        out.greeks.delta = opt.notional * (values_up[0] - values_down[0]) / (2.0 * dS);
        out.greeks.gamma = opt.notional * (values_up[0] - 2.0 * values[0] + values_down[0]) / (dS * dS);

        // Vega: bump volatility by 1%
        const Real dsigma = 0.01;
        const Real sigma_bump = sigma + dsigma;
        const Real u_bump = std::exp(sigma_bump * std::sqrt(dt));
        const Real d_bump = 1.0 / u_bump;
        const Real p_bump = (a - d_bump) / (u_bump - d_bump);

        std::vector<Real> values_vega(steps_ + 1);
        for (int j = 0; j <= steps_; ++j)
        {
            const Real ST = S0 * std::pow(u_bump, j) * std::pow(d_bump, steps_ - j);
            values_vega[j] = (*opt.payoff)(ST);
        }
        for (int i = steps_ - 1; i >= 0; --i)
        {
            for (int j = 0; j <= i; ++j)
            {
                const Real S = S0 * std::pow(u_bump, j) * std::pow(d_bump, i - j);
                const Real continuation = df * (p_bump * values_vega[j + 1] + (1.0 - p_bump) * values_vega[j]);
                if (is_american)
                {
                    const Real exercise = (*opt.payoff)(S);
                    values_vega[j] = std::max(continuation, exercise);
                }
                else
                {
                    values_vega[j] = continuation;
                }
            }
        }
        out.greeks.vega = opt.notional * (values_vega[0] - values[0]) / dsigma;

        // Theta: approximate by stepping one time step forward
        if (steps_ > 1)
        {
            const Real T_minus_dt = T - dt;
            const int steps_minus_1 = steps_ - 1;
            const Real dt_theta = T_minus_dt / steps_minus_1;
            const Real u_theta = std::exp(sigma * std::sqrt(dt_theta));
            const Real d_theta = 1.0 / u_theta;
            const Real a_theta = std::exp((r - q) * dt_theta);
            const Real p_theta = (a_theta - d_theta) / (u_theta - d_theta);
            const Real df_theta = std::exp(-r * dt_theta);

            std::vector<Real> values_theta(steps_);
            for (int j = 0; j < steps_; ++j)
            {
                const Real ST = S0 * std::pow(u_theta, j) * std::pow(d_theta, steps_minus_1 - j);
                values_theta[j] = (*opt.payoff)(ST);
            }
            for (int i = steps_minus_1 - 1; i >= 0; --i)
            {
                for (int j = 0; j <= i; ++j)
                {
                    const Real S = S0 * std::pow(u_theta, j) * std::pow(d_theta, i - j);
                    const Real continuation = df_theta * (p_theta * values_theta[j + 1] + (1.0 - p_theta) * values_theta[j]);
                    if (is_american)
                    {
                        const Real exercise = (*opt.payoff)(S);
                        values_theta[j] = std::max(continuation, exercise);
                    }
                    else
                    {
                        values_theta[j] = continuation;
                    }
                }
            }
            out.greeks.theta = -opt.notional * (values[0] - values_theta[0]) / dt;
        }

        res_ = out;
    }

    void BinomialVanillaEngine::visit(const AsianOption &)
    {
        throw UnsupportedInstrument("BinomialVanillaEngine does not support Asian options.");
    }

    void BinomialVanillaEngine::visit(const EquityFuture &)
    {
        throw UnsupportedInstrument("BinomialVanillaEngine does not support equity futures.");
    }

    void BinomialVanillaEngine::visit(const ZeroCouponBond &)
    {
        throw UnsupportedInstrument("BinomialVanillaEngine does not support bonds.");
    }

    void BinomialVanillaEngine::visit(const FixedRateBond &)
    {
        throw UnsupportedInstrument("BinomialVanillaEngine does not support bonds.");
    }

    void BinomialVanillaEngine::validate(const VanillaOption &opt)
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
