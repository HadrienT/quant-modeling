#include "quantModeling/engines/mc/basket.hpp"

#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"

#include <Eigen/Core>
#include <cmath>
#include <numeric>
#include <string>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────
    void BSBasketMCEngine::validate(const BasketOption &opt, int n_assets, int n_paths)
    {
        if (!opt.payoff)
            throw InvalidInput("BasketOption: payoff is null");
        if (!opt.exercise || opt.exercise->dates().empty())
            throw InvalidInput("BasketOption: exercise is null or has no dates");
        if (opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument("BSBasketMCEngine: only European exercise is supported");
        if (opt.exercise->dates().front() <= 0.0)
            throw InvalidInput("BasketOption: maturity must be > 0");
        if (opt.notional == 0.0)
            throw InvalidInput("BasketOption: notional must be non-zero");
        if (n_paths <= 0)
            throw InvalidInput("BasketOption: mc_paths must be > 0");
        if (n_assets < 2)
            throw InvalidInput("BasketOption: at least 2 assets required");
        if (static_cast<int>(opt.weights.size()) != n_assets)
            throw InvalidInput("BasketOption: weights.size() != n_assets");
    }

    // ─────────────────────────────────────────────────────────────────────
    void BSBasketMCEngine::visit(const BasketOption &opt)
    {
        const auto &m = require_model<MultiAssetBSModel>("BSBasketMCEngine");
        const PricingSettings settings = ctx_.settings;
        const int n = m.n_assets();
        validate(opt, n, settings.mc_paths);

        // ── Model parameters ──────────────────────────────────────────
        const Real r = m.rate_r;
        const Real T = opt.exercise->dates().front();
        const Real K = opt.payoff->strike();
        const bool is_call = (opt.payoff->type() == OptionType::Call);
        const Real df = std::exp(-r * T);

        // Per-asset: drift_i = (r - q_i - 0.5*sigma_i^2)*T,  sv_i = sigma_i*sqrt(T)
        Eigen::VectorXd mu(n), sv(n);
        for (int k = 0; k < n; ++k)
        {
            mu[k] = (r - m.dividends[k] - 0.5 * m.vols[k] * m.vols[k]) * T;
            sv[k] = m.vols[k] * std::sqrt(T);
        }

        // Weights vector
        Eigen::VectorXd W(n);
        for (int k = 0; k < n; ++k)
            W[k] = opt.weights[k];

        // Initial basket value B0 = sum_i(w_i * S0_i)
        double B0 = 0.0;
        for (int k = 0; k < n; ++k)
            B0 += opt.weights[k] * m.spots[k];

        // ── FD bump sizes (common random numbers for greeks) ──────────
        const GreeksBumps bumps;

        // Gamma: parallel scale all S0_i by (1 +/- eps).
        // Since ST_i = S0_i * exp(...), scaling S0_i scales ST_i → basket scales too.
        const Real dS = B0 * bumps.delta_bump; // absolute bump on basket
        const Real sc_up = 1.0 + bumps.delta_bump;
        const Real sc_dn = 1.0 - bumps.delta_bump;

        // Vega: parallel shift all sigmas by +/- eps_v (flat vol surface shift)
        const Real eps_v = bumps.vega_bump;
        Eigen::VectorXd mu_vup(n), sv_vup(n), mu_vdn(n), sv_vdn(n);
        for (int k = 0; k < n; ++k)
        {
            const Real s_up = m.vols[k] + eps_v;
            const Real s_dn = m.vols[k] - eps_v;
            mu_vup[k] = (r - m.dividends[k] - 0.5 * s_up * s_up) * T;
            sv_vup[k] = s_up * std::sqrt(T);
            mu_vdn[k] = (r - m.dividends[k] - 0.5 * s_dn * s_dn) * T;
            sv_vdn[k] = s_dn * std::sqrt(T);
        }

        // Rho: rate bump (same Cholesky, vols unchanged — only drift and df change)
        const Real eps_r = bumps.rho_bump;
        const Real r_up = r + eps_r;
        const Real r_dn = r - eps_r;
        const Real df_rup = std::exp(-r_up * T);
        const Real df_rdn = std::exp(-r_dn * T);
        Eigen::VectorXd mu_rup(n), mu_rdn(n);
        for (int k = 0; k < n; ++k)
        {
            mu_rup[k] = (r_up - m.dividends[k] - 0.5 * m.vols[k] * m.vols[k]) * T;
            mu_rdn[k] = (r_dn - m.dividends[k] - 0.5 * m.vols[k] * m.vols[k]) * T;
        }

        // Theta: T bump
        const Real eps_T = bumps.theta_bump;
        const Real T_up = T + eps_T;
        const Real T_dn = std::max(1e-8, T - eps_T);
        const Real df_Tup = std::exp(-r * T_up);
        const Real df_Tdn = std::exp(-r * T_dn);
        Eigen::VectorXd mu_Tup(n), sv_Tup(n), mu_Tdn(n), sv_Tdn(n);
        for (int k = 0; k < n; ++k)
        {
            const Real base_drift = r - m.dividends[k] - 0.5 * m.vols[k] * m.vols[k];
            mu_Tup[k] = base_drift * T_up;
            mu_Tdn[k] = base_drift * T_dn;
            sv_Tup[k] = m.vols[k] * std::sqrt(T_up);
            sv_Tdn[k] = m.vols[k] * std::sqrt(T_dn);
        }

        // ── Payoff lambda ─────────────────────────────────────────────
        auto payoff = [&](double basket_val) -> double
        {
            return is_call ? std::max(basket_val - K, 0.0)
                           : std::max(K - basket_val, 0.0);
        };

        // Basket terminal value from per-asset exponent vectors and a given z
        auto basket_from_z = [&](const Eigen::VectorXd &mu_v,
                                 const Eigen::VectorXd &sv_v,
                                 const Eigen::VectorXd &z) -> double
        {
            double B = 0.0;
            for (int k = 0; k < n; ++k)
                B += W[k] * m.spots[k] * std::exp(mu_v[k] + sv_v[k] * z[k]);
            return B;
        };

        // ── Welford state ─────────────────────────────────────────────
        Real meanPV = 0.0, M2_pv = 0.0;
        Real meanDelta = 0.0, M2_delta = 0.0;
        Real meanGamma = 0.0, M2_gamma = 0.0;
        Real meanVega = 0.0, M2_vega = 0.0;
        Real meanRho = 0.0, M2_rho = 0.0;
        Real meanTheta = 0.0, M2_theta = 0.0;
        int N = 0;

        auto welford = [](Real x, Real &mean, Real &M2, int n_val)
        {
            const Real d = x - mean;
            mean += d / static_cast<Real>(n_val);
            M2 += d * (x - mean);
        };

        // ── RNG ───────────────────────────────────────────────────────
        RngFactory rng_factory(static_cast<uint64_t>(settings.mc_seed));
        Pcg32 rng = rng_factory.make(0);
        NormalBoxMuller bm;

        Eigen::VectorXd u_prev(n); // stored so odd paths can use -u_prev (antithetic)

        // ── Monte Carlo loop ──────────────────────────────────────────
        for (int i = 0; i < settings.mc_paths; ++i)
        {
            // ── Draw correlated normals ───────────────────────────────
            const bool use_antithetic = settings.mc_antithetic && ((i & 1) == 1);
            Eigen::VectorXd u(n);
            if (!use_antithetic)
            {
                for (int k = 0; k < n; ++k)
                    u_prev[k] = bm(rng);
                u = u_prev;
            }
            else
            {
                u = -u_prev;
            }
            // z = L * u  (correlated Gaussians: Cov(z) = L L^T = C)
            const Eigen::VectorXd z = m.chol * u;

            // ── Base path ─────────────────────────────────────────────
            const double B = basket_from_z(mu, sv, z);
            const double pv = payoff(B);

            // ── Pathwise delta ────────────────────────────────────────
            // delta = sum_i dV/dS0_i = df * sum_i(w_i * ST_i/S0_i) * 1_{ITM}
            double delta_pw = 0.0;
            if (pv > 0.0)
            {
                for (int k = 0; k < n; ++k)
                    delta_pw += W[k] * (m.spots[k] * std::exp(mu[k] + sv[k] * z[k])) / m.spots[k];
                delta_pw *= df;
            }

            // ── FD-CRN Gamma (parallel spot scale) ───────────────────
            // Scaling all S0_i by sc scales all ST_i (and B) by sc → CRN exact.
            const double pv_up = payoff(B * sc_up);
            const double pv_dn = payoff(B * sc_dn);
            const double gamma_pw = df * (pv_up - 2.0 * pv + pv_dn) / (dS * dS);

            // ── FD-CRN Vega (parallel vol shift) ─────────────────────
            const double B_vup = basket_from_z(mu_vup, sv_vup, z);
            const double B_vdn = basket_from_z(mu_vdn, sv_vdn, z);
            const double vega_pw = df * (payoff(B_vup) - payoff(B_vdn)) / (2.0 * eps_v);

            // ── FD-CRN Rho (rate shift) ───────────────────────────────
            const double B_rup = basket_from_z(mu_rup, sv, z);
            const double B_rdn = basket_from_z(mu_rdn, sv, z);
            const double rho_pw = (df_rup * payoff(B_rup) - df_rdn * payoff(B_rdn)) / (2.0 * eps_r);

            // ── FD-CRN Theta (maturity shift) ────────────────────────
            const double B_Tup = basket_from_z(mu_Tup, sv_Tup, z);
            const double B_Tdn = basket_from_z(mu_Tdn, sv_Tdn, z);
            const double theta_pw = -(df_Tup * payoff(B_Tup) - df_Tdn * payoff(B_Tdn)) / (2.0 * eps_T);

            // ── Accumulate ────────────────────────────────────────────
            ++N;
            welford(pv, meanPV, M2_pv, N);
            welford(delta_pw, meanDelta, M2_delta, N);
            welford(gamma_pw, meanGamma, M2_gamma, N);
            welford(vega_pw, meanVega, M2_vega, N);
            welford(rho_pw, meanRho, M2_rho, N);
            welford(theta_pw, meanTheta, M2_theta, N);
        }

        // ── Assemble result ───────────────────────────────────────────
        auto std_err = [&](Real M2_val) -> Real
        {
            return (N > 1) ? std::sqrt((M2_val / static_cast<Real>(N - 1)) / static_cast<Real>(N))
                           : 0.0;
        };

        PricingResult out;
        out.npv = opt.notional * df * meanPV;
        out.mc_std_error = opt.notional * df * std_err(M2_pv);

        out.greeks.delta = opt.notional * meanDelta;
        out.greeks.delta_std_error = opt.notional * std_err(M2_delta);
        out.greeks.gamma = opt.notional * meanGamma;
        out.greeks.gamma_std_error = opt.notional * std_err(M2_gamma);
        out.greeks.vega = opt.notional * df * meanVega;
        out.greeks.vega_std_error = opt.notional * df * std_err(M2_vega);
        out.greeks.rho = opt.notional * meanRho;
        out.greeks.rho_std_error = opt.notional * std_err(M2_rho);
        out.greeks.theta = opt.notional * meanTheta;
        out.greeks.theta_std_error = opt.notional * std_err(M2_theta);

        // Build asset summary for diagnostics
        std::string asset_info;
        for (int k = 0; k < n; ++k)
        {
            asset_info += "S" + std::to_string(k) + "=" + std::to_string(m.spots[k]) + "/v=" + std::to_string(m.vols[k]) + "/w=" + std::to_string(opt.weights[k]);
            if (k < n - 1)
                asset_info += " ";
        }

        out.diagnostics =
            std::string("BS MC European Basket") +
            (settings.mc_antithetic ? " + antithetic" : "") +
            ": n_assets=" + std::to_string(n) +
            ", K=" + std::to_string(K) +
            ", T=" + std::to_string(T) +
            ", paths=" + std::to_string(settings.mc_paths) +
            " | " + asset_info;

        res_ = out;
    }

} // namespace quantModeling
