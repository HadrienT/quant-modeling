#include "quantModeling/engines/mc/local_vol.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/volatility.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace quantModeling
{

    namespace // internal linkage
    {

        // ─────────────────────────────────────────────────────────────────────────────
        //  Single-pass log-Euler simulation using GridLocalVol.
        //  Returns a vector of terminal spot values (size n_paths).
        // ─────────────────────────────────────────────────────────────────────────────
        std::vector<Real> simulate(
            const GridLocalVol &lv,
            Real S0,
            Real r,
            Real q,
            Real T,
            int n_paths,
            int n_steps,
            bool antithetic,
            uint64_t seed)
        {
            const Real dt = T / static_cast<Real>(n_steps);
            const Real sqdt = std::sqrt(dt);

            const int half = antithetic ? n_paths / 2 : n_paths;
            const int total = antithetic ? 2 * half : n_paths;

            std::vector<Real> S_T(static_cast<size_t>(total));

            RngFactory rngFact(seed);
            NormalBoxMuller gauss;

            // We maintain two parallel spot vectors: base path and antithetic path
            std::vector<Real> S_base(static_cast<size_t>(half), S0);
            std::vector<Real> S_anti(antithetic ? static_cast<size_t>(half) : 0u, S0);

            for (int step = 0; step < n_steps; ++step)
            {
                const Real t_cur = static_cast<Real>(step) * dt;
                Pcg32 rng = rngFact.make(static_cast<uint64_t>(step)); // reproducible stream per step

                for (int p = 0; p < half; ++p)
                {
                    const Real z = gauss(rng);

                    // Base path
                    {
                        const Real sig = lv.value(S_base[static_cast<size_t>(p)], t_cur);
                        const Real drift = (r - q - 0.5 * sig * sig) * dt;
                        S_base[static_cast<size_t>(p)] *= std::exp(drift + sig * sqdt * z);
                    }

                    // Antithetic path (same |z|, opposite sign)
                    if (antithetic)
                    {
                        const Real sig = lv.value(S_anti[static_cast<size_t>(p)], t_cur);
                        const Real drift = (r - q - 0.5 * sig * sig) * dt;
                        S_anti[static_cast<size_t>(p)] *= std::exp(drift + sig * sqdt * (-z));
                    }
                }
            }

            for (int p = 0; p < half; ++p)
                S_T[static_cast<size_t>(p)] = S_base[static_cast<size_t>(p)];

            if (antithetic)
                for (int p = 0; p < half; ++p)
                    S_T[static_cast<size_t>(half + p)] = S_anti[static_cast<size_t>(p)];

            return S_T;
        }

        // ─────────────────────────────────────────────────────────────────────────────
        //  Discounted payoff mean and standard error
        // ─────────────────────────────────────────────────────────────────────────────
        std::pair<Real, Real> payoff_stats(
            const std::vector<Real> &S_T,
            Real K,
            Real T,
            Real r,
            bool is_call)
        {
            const DiscountCurve disc(r);
            const Real df = disc.discount(T);
            const int n = static_cast<int>(S_T.size());

            Real mean = 0.0, M2 = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const Real pay = is_call
                                     ? std::max(S_T[static_cast<size_t>(i)] - K, 0.0)
                                     : std::max(K - S_T[static_cast<size_t>(i)], 0.0);
                const Real x = df * pay;
                const Real delta = x - mean;
                mean += delta / static_cast<Real>(i + 1);
                M2 += delta * (x - mean);
            }
            const Real variance = (n > 1) ? M2 / static_cast<Real>(n - 1) : 0.0;
            const Real se = std::sqrt(variance / static_cast<Real>(n));
            return {mean, se};
        }

    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────────
    //  Public entry point
    // ─────────────────────────────────────────────────────────────────────────────
    PricingResult price_local_vol_mc(const LocalVolInput &in)
    {
        // Basic validation
        if (in.K_grid.empty() || in.T_grid.empty() || in.sigma_loc_flat.empty())
            throw std::invalid_argument("price_local_vol_mc: local-vol grid is empty.");

        const size_t nK = in.K_grid.size();
        const size_t nT = in.T_grid.size();
        if (in.sigma_loc_flat.size() != nK * nT)
            throw std::invalid_argument(
                "price_local_vol_mc: sigma_loc_flat.size() != K_grid.size() * T_grid.size().");

        if (in.maturity <= 0.0)
            throw std::invalid_argument("price_local_vol_mc: maturity must be > 0.");

        const int n_steps = std::max(
            static_cast<int>(std::ceil(in.maturity * static_cast<Real>(in.n_steps_per_year))), 1);

        const int n_paths = in.mc_antithetic ? (in.n_paths / 2) * 2 : in.n_paths;

        const GreeksBumps bumps;
        const Real dS = in.spot * bumps.delta_bump; // 1% spot bump
        const Real eps_r = bumps.rho_bump;          // 1 bp
        const Real eps_T = bumps.theta_bump;        // 1 day
        const Real eps_v = 0.01;                    // 1% parallel vol shift

        // Base grid (no shift) — GridLocalVol copies the data vectors once.
        GridLocalVol lv_base(in.K_grid, in.T_grid, in.sigma_loc_flat, 0.0);

        // ── Base price ──────────────────────────────────────────────────────────
        auto ST_base = simulate(lv_base, in.spot, in.rate, in.dividend,
                                in.maturity, n_paths, n_steps, in.mc_antithetic,
                                static_cast<uint64_t>(in.seed));
        auto [npv, se] = payoff_stats(ST_base, in.strike, in.maturity, in.rate, in.is_call);

        PricingResult res;
        res.npv = npv;
        res.mc_std_error = se;

        {
            std::ostringstream oss;
            oss << "LocalVolMC paths=" << n_paths << " steps=" << n_steps
                << " antithetic=" << (in.mc_antithetic ? "true" : "false");
            res.diagnostics = oss.str();
        }

        if (!in.compute_greeks)
            return res;

        // ── Delta & Gamma (CRN, central difference on spot) ──────────────────────
        auto ST_up = simulate(lv_base, in.spot + dS, in.rate, in.dividend,
                              in.maturity, n_paths, n_steps, in.mc_antithetic,
                              static_cast<uint64_t>(in.seed));
        auto ST_dn = simulate(lv_base, in.spot - dS, in.rate, in.dividend,
                              in.maturity, n_paths, n_steps, in.mc_antithetic,
                              static_cast<uint64_t>(in.seed));

        auto [npv_up, se_up] = payoff_stats(ST_up, in.strike, in.maturity, in.rate, in.is_call);
        auto [npv_dn, se_dn] = payoff_stats(ST_dn, in.strike, in.maturity, in.rate, in.is_call);

        res.greeks.delta = (npv_up - npv_dn) / (2.0 * dS);
        res.greeks.gamma = (npv_up - 2.0 * npv + npv_dn) / (dS * dS);

        // Delta std error via error propagation: Δse ≈ sqrt(se_up² + se_dn²) / (2·dS)
        res.greeks.delta_std_error = std::sqrt(se_up * se_up + se_dn * se_dn) / (2.0 * dS);
        res.greeks.gamma_std_error = std::sqrt(se_up * se_up + 4.0 * se * se + se_dn * se_dn) / (dS * dS);

        // ── Theta (forward difference: shorten maturity by 1 day) ────────────────
        const Real mat_short = std::max(in.maturity - eps_T, 1.0 / 365.0);
        const int n_steps_short = std::max(
            static_cast<int>(std::ceil(mat_short * static_cast<Real>(in.n_steps_per_year))), 1);

        auto ST_short = simulate(lv_base, in.spot, in.rate, in.dividend,
                                 mat_short, n_paths, n_steps_short, in.mc_antithetic,
                                 static_cast<uint64_t>(in.seed));
        auto [npv_short, se_short] = payoff_stats(ST_short, in.strike, mat_short, in.rate, in.is_call);

        res.greeks.theta = (npv_short - npv) / eps_T;
        res.greeks.theta_std_error = std::sqrt(se_short * se_short + se * se) / eps_T;

        // ── Rho (bump rate by +1 bp) ──────────────────────────────────────────────
        auto ST_rho = simulate(lv_base, in.spot, in.rate + eps_r, in.dividend,
                               in.maturity, n_paths, n_steps, in.mc_antithetic,
                               static_cast<uint64_t>(in.seed));
        auto [npv_rho, se_rho] = payoff_stats(ST_rho, in.strike, in.maturity, in.rate + eps_r, in.is_call);

        res.greeks.rho = (npv_rho - npv) / eps_r;
        res.greeks.rho_std_error = std::sqrt(se_rho * se_rho + se * se) / eps_r;

        // ── Vega (parallel +1% shift on σ_loc grid, CRN) ─────────────────────────
        const GridLocalVol lv_vshift = lv_base.shifted(eps_v);

        auto ST_vega = simulate(lv_vshift, in.spot, in.rate, in.dividend,
                                in.maturity, n_paths, n_steps, in.mc_antithetic,
                                static_cast<uint64_t>(in.seed));
        auto [npv_vega, se_vega] = payoff_stats(ST_vega, in.strike, in.maturity, in.rate, in.is_call);

        res.greeks.vega = (npv_vega - npv) / eps_v;
        res.greeks.vega_std_error = std::sqrt(se_vega * se_vega + se * se) / eps_v;

        return res;
    }

} // namespace quantModeling
