#include "quantModeling/engines/pde/european_vanilla.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace quantModeling
{
    PDEEuropeanVanillaEngine::PDEEuropeanVanillaEngine(PricingContext ctx)
        : EngineBase(std::move(ctx)),
          M_(ctx_.settings.pde_space_steps),
          N_(ctx_.settings.pde_time_steps)
    {
        if (M_ < 2)
            throw InvalidInput("PDE requires space_steps >= 2");
        if (N_ < 1)
            throw InvalidInput("PDE requires time_steps >= 1");
    }

    void PDEEuropeanVanillaEngine::visit(const VanillaOption &opt)
    {
        validate(opt);
        const auto &m = require_model<ILocalVolModel>("PDEEuropeanVanillaEngine");

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();
        const Real T = opt.exercise->dates().front();
        const Real K = opt.payoff->strike();
        const OptionType type = opt.payoff->type();

        // Grid setup
        const Real dt = T / N_;

        // Space grid: log-transformed coordinates
        // x = ln(S/K), so S = K * exp(x)
        const Real x_min = -1.0; // S = K * exp(-1) ≈ 0.37 * K
        const Real x_max = 1.0;  // S = K * exp(1) ≈ 2.72 * K
        const Real dx = (x_max - x_min) / M_;

        std::vector<Real> V(M_ + 1);
        std::vector<Real> V_new(M_ + 1);
        std::vector<Real> payoff(M_ + 1);

        // Precompute stock prices and payoffs at grid points
        std::vector<Real> S_grid(M_ + 1);
        for (int j = 0; j <= M_; ++j)
        {
            const Real x = x_min + j * dx;
            S_grid[j] = K * std::exp(x);
            payoff[j] = (*opt.payoff)(S_grid[j]);
        }

        // Terminal condition: V(S, T) = payoff(S)
        for (int j = 0; j <= M_; ++j)
        {
            V[j] = payoff[j];
        }

        // Crank-Nicolson coefficients
        // In log-space x = ln(S/K), the PDE becomes:
        // dV/dt = 0.5 * sigma^2 * d2V/dx2 + (r - q - 0.5 * sigma^2) * dV/dx - r * V

        const Real drift = r - q - 0.5 * sigma * sigma;
        const Real alpha = 0.5 * sigma * sigma;

        const Real lambda = dt / (dx * dx);
        const Real mu = dt / (2.0 * dx);

        std::vector<Real> a(M_ + 1);
        std::vector<Real> b(M_ + 1);
        std::vector<Real> c(M_ + 1);
        std::vector<Real> d(M_ + 1);

        // Backward in time (from T to 0)
        for (int n = N_ - 1; n >= 0; --n)
        {
            // Build RHS: (I + 0.5*dt*L)*V
            for (int j = 1; j < M_; ++j)
            {
                const Real coeff_jm1 = alpha * lambda - drift * mu;
                const Real coeff_j = 1.0 - alpha * 2.0 * lambda - 0.5 * r * dt;
                const Real coeff_jp1 = alpha * lambda + drift * mu;

                d[j] = coeff_jm1 * V[j - 1] + coeff_j * V[j] + coeff_jp1 * V[j + 1];
            }

            // Boundary conditions
            const Real df = std::exp(-r * (T - n * dt));
            if (type == OptionType::Call)
            {
                d[0] = 0.0;
                d[M_] = S_grid[M_] - K * df;
            }
            else
            {
                d[0] = K * df;
                d[M_] = 0.0;
            }

            // Build LHS matrix: (I - 0.5*dt*L)
            for (int j = 1; j < M_; ++j)
            {
                a[j] = -(alpha * lambda - drift * mu) * 0.5;
                b[j] = 1.0 + alpha * 2.0 * lambda * 0.5 + 0.5 * r * dt;
                c[j] = -(alpha * lambda + drift * mu) * 0.5;
            }

            // Boundary conditions for matrix
            b[0] = 1.0;
            c[0] = 0.0;
            a[M_] = 0.0;
            b[M_] = 1.0;

            // Solve tridiagonal system
            solve_tridiagonal(a, b, c, d, V_new);
            V = V_new;
        }

        // Interpolate to find V(S0, 0)
        const Real x0 = std::log(S0 / K);
        Real npv = 0.0;

        if (x0 <= x_min)
        {
            npv = V[0];
        }
        else if (x0 >= x_max)
        {
            npv = V[M_];
        }
        else
        {
            const int j_left = static_cast<int>((x0 - x_min) / dx);
            const int j_right = j_left + 1;
            const Real x_left = x_min + j_left * dx;
            const Real x_right = x_min + j_right * dx;
            const Real weight = (x0 - x_left) / (x_right - x_left);
            npv = (1.0 - weight) * V[j_left] + weight * V[j_right];
        }

        PricingResult out;
        out.npv = opt.notional * npv;
        out.diagnostics = "PDE Crank-Nicolson European vanilla (M=" + std::to_string(M_) +
                          ", N=" + std::to_string(N_) + ")";

        // Greeks via finite differences
        const Real dS = S0 * 0.01;

        // Delta computation (spot bump)
        {
            std::vector<Real> V_temp(M_ + 1);
            std::vector<Real> V_new_temp(M_ + 1);
            for (int j = 0; j <= M_; ++j)
            {
                const Real x = x_min + j * dx;
                const Real S = (S0 + dS) * std::exp(x - std::log(S0));
                V_temp[j] = (*opt.payoff)(S);
            }
            for (int n = N_ - 1; n >= 0; --n)
            {
                for (int j = 1; j < M_; ++j)
                {
                    const Real coeff_jm1 = alpha * lambda - drift * mu;
                    const Real coeff_j = 1.0 - alpha * 2.0 * lambda - 0.5 * r * dt;
                    const Real coeff_jp1 = alpha * lambda + drift * mu;
                    d[j] = coeff_jm1 * V_temp[j - 1] + coeff_j * V_temp[j] + coeff_jp1 * V_temp[j + 1];
                }
                const Real df = std::exp(-r * (T - n * dt));
                if (type == OptionType::Call)
                {
                    d[0] = 0.0;
                    d[M_] = S_grid[M_] * (S0 + dS) / S0 - K * df;
                }
                else
                {
                    d[0] = K * df;
                    d[M_] = 0.0;
                }
                for (int j = 1; j < M_; ++j)
                {
                    a[j] = -(alpha * lambda - drift * mu) * 0.5;
                    b[j] = 1.0 + alpha * 2.0 * lambda * 0.5 + 0.5 * r * dt;
                    c[j] = -(alpha * lambda + drift * mu) * 0.5;
                }
                b[0] = 1.0;
                c[0] = 0.0;
                a[M_] = 0.0;
                b[M_] = 1.0;
                solve_tridiagonal(a, b, c, d, V_new_temp);
                V_temp = V_new_temp;
            }

            Real x0_up = std::log((S0 + dS) / K);
            Real npv_up = 0.0;
            if (x0_up <= x_min)
                npv_up = V_temp[0];
            else if (x0_up >= x_max)
                npv_up = V_temp[M_];
            else
            {
                int j = static_cast<int>((x0_up - x_min) / dx);
                Real w = (x0_up - (x_min + j * dx)) / dx;
                npv_up = (1.0 - w) * V_temp[j] + w * V_temp[j + 1];
            }

            // Delta for down
            for (int j = 0; j <= M_; ++j)
            {
                const Real x = x_min + j * dx;
                const Real S = (S0 - dS) * std::exp(x - std::log(S0));
                V_temp[j] = (*opt.payoff)(S);
            }
            for (int n = N_ - 1; n >= 0; --n)
            {
                for (int j = 1; j < M_; ++j)
                {
                    const Real coeff_jm1 = alpha * lambda - drift * mu;
                    const Real coeff_j = 1.0 - alpha * 2.0 * lambda - 0.5 * r * dt;
                    const Real coeff_jp1 = alpha * lambda + drift * mu;
                    d[j] = coeff_jm1 * V_temp[j - 1] + coeff_j * V_temp[j] + coeff_jp1 * V_temp[j + 1];
                }
                const Real df = std::exp(-r * (T - n * dt));
                if (type == OptionType::Call)
                {
                    d[0] = 0.0;
                    d[M_] = S_grid[M_] * (S0 - dS) / S0 - K * df;
                }
                else
                {
                    d[0] = K * df;
                    d[M_] = 0.0;
                }
                for (int j = 1; j < M_; ++j)
                {
                    a[j] = -(alpha * lambda - drift * mu) * 0.5;
                    b[j] = 1.0 + alpha * 2.0 * lambda * 0.5 + 0.5 * r * dt;
                    c[j] = -(alpha * lambda + drift * mu) * 0.5;
                }
                b[0] = 1.0;
                c[0] = 0.0;
                a[M_] = 0.0;
                b[M_] = 1.0;
                solve_tridiagonal(a, b, c, d, V_new_temp);
                V_temp = V_new_temp;
            }

            Real x0_down = std::log((S0 - dS) / K);
            Real npv_down = 0.0;
            if (x0_down <= x_min)
                npv_down = V_temp[0];
            else if (x0_down >= x_max)
                npv_down = V_temp[M_];
            else
            {
                int j = static_cast<int>((x0_down - x_min) / dx);
                Real w = (x0_down - (x_min + j * dx)) / dx;
                npv_down = (1.0 - w) * V_temp[j] + w * V_temp[j + 1];
            }

            out.greeks.delta = opt.notional * (npv_up - npv_down) / (2.0 * dS);
            out.greeks.gamma = opt.notional * (npv_up - 2.0 * npv + npv_down) / (dS * dS);
        }

        res_ = out;
    }

    void PDEEuropeanVanillaEngine::visit(const AsianOption &)
    {
        throw UnsupportedInstrument("PDEEuropeanVanillaEngine does not support Asian options.");
    }

    void PDEEuropeanVanillaEngine::visit(const EquityFuture &)
    {
        throw UnsupportedInstrument("PDEEuropeanVanillaEngine does not support equity futures.");
    }

    void PDEEuropeanVanillaEngine::visit(const ZeroCouponBond &)
    {
        throw UnsupportedInstrument("PDEEuropeanVanillaEngine does not support bonds.");
    }

    void PDEEuropeanVanillaEngine::visit(const FixedRateBond &)
    {
        throw UnsupportedInstrument("PDEEuropeanVanillaEngine does not support bonds.");
    }

    void PDEEuropeanVanillaEngine::validate(const VanillaOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("VanillaOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("VanillaOption.exercise is null");
        if (opt.exercise->type() != ExerciseType::European)
        {
            throw UnsupportedInstrument("PDE engine is designed for European exercise only");
        }
        if (opt.exercise->dates().size() != 1)
        {
            throw InvalidInput("EuropeanExercise must contain exactly one date (maturity)");
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

    void PDEEuropeanVanillaEngine::solve_tridiagonal(const std::vector<Real> &a,
                                                     const std::vector<Real> &b,
                                                     const std::vector<Real> &c,
                                                     const std::vector<Real> &d,
                                                     std::vector<Real> &x)
    {
        const int n = static_cast<int>(b.size());
        x.resize(n);

        std::vector<Real> c_star(n);
        std::vector<Real> d_star(n);

        // Forward sweep
        c_star[0] = c[0] / b[0];
        d_star[0] = d[0] / b[0];

        for (int i = 1; i < n; ++i)
        {
            const Real denom = b[i] - a[i] * c_star[i - 1];
            c_star[i] = c[i] / denom;
            d_star[i] = (d[i] - a[i] * d_star[i - 1]) / denom;
        }

        // Back substitution
        x[n - 1] = d_star[n - 1];
        for (int i = n - 2; i >= 0; --i)
        {
            x[i] = d_star[i] - c_star[i] * x[i + 1];
        }
    }
} // namespace quantModeling
