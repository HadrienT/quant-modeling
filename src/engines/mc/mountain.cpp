#include "quantModeling/engines/mc/mountain.hpp"

#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────
    //  Himalaya (Mountain) MC pricing
    // ─────────────────────────────────────────────────────────────────────

    void BSMountainMCEngine::visit(const MountainOption &opt)
    {
        const auto &m = require_model<MultiAssetBSModel>("BSMountainMCEngine");
        const PricingSettings &settings = ctx_.settings;

        const auto n = static_cast<std::size_t>(m.n_assets());
        const auto n_obs = opt.observation_dates.size();

        // ── Validate ──────────────────────────────────────────────────
        if (n_obs != n)
            throw InvalidInput("MountainOption: observation_dates.size() must equal n_assets");
        if (n < 2)
            throw InvalidInput("MountainOption: need at least 2 assets");
        if (opt.notional == 0.0)
            throw InvalidInput("MountainOption: notional must be non-zero");
        for (std::size_t i = 0; i < n_obs; ++i)
        {
            if (opt.observation_dates[i] <= 0.0)
                throw InvalidInput("MountainOption: observation dates must be > 0");
            if (i > 0 && opt.observation_dates[i] <= opt.observation_dates[i - 1])
                throw InvalidInput("MountainOption: observation dates must be strictly increasing");
        }

        const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 42);

        const Real r = m.rate_r;
        const Real T_final = opt.observation_dates.back();
        const Real df = m.discount_curve().discount(T_final);

        // ── Precompute per-step drift and vol for each asset ─────────
        // Between observation dates, each asset evolves as:
        //   S_j(T_i) = S_j(T_{i-1}) × exp(μ_j dt + σ_j √dt z_j)
        //   μ_j = r − q_j − 0.5σ²_j
        struct AssetSpec
        {
            Real base_drift; // (r − q − 0.5σ²)
            Real vol;        // σ
        };
        std::vector<AssetSpec> asset_specs(n);
        for (std::size_t j = 0; j < n; ++j)
        {
            asset_specs[j].base_drift = r - m.dividends[j] - 0.5 * m.vols[j] * m.vols[j];
            asset_specs[j].vol = m.vols[j];
        }

        struct StepSpec
        {
            Real dt;
            Real sqrt_dt;
        };
        std::vector<StepSpec> step_specs(n_obs);
        {
            Real t_prev = 0.0;
            for (std::size_t i = 0; i < n_obs; ++i)
            {
                const Real dt = opt.observation_dates[i] - t_prev;
                step_specs[i].dt = dt;
                step_specs[i].sqrt_dt = std::sqrt(dt);
                t_prev = opt.observation_dates[i];
            }
        }

        // ── Cholesky factor from model ───────────────────────────────
        const Eigen::MatrixXd &L = m.chol; // lower-triangular

        // ── Monte Carlo loop ─────────────────────────────────────────
        RngFactory rng_fact(seed);

        Real sum = 0.0;
        Real sum2 = 0.0;

        // Workspace vectors — allocated once, reused per path
        Eigen::VectorXd u(static_cast<Eigen::Index>(n));
        Eigen::VectorXd z(static_cast<Eigen::Index>(n));
        std::vector<Real> S(n);     // current spot per asset
        std::vector<bool> alive(n); // which assets are still in basket

        for (int p = 0; p < n_paths; ++p)
        {
            Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
            NormalBoxMuller normal;

            // Reset spots
            for (std::size_t j = 0; j < n; ++j)
            {
                S[j] = m.spots[j];
                alive[j] = true;
            }

            Real sum_perf = 0.0;

            for (std::size_t i = 0; i < n_obs; ++i)
            {
                // Draw i.i.d. normals → correlated via Cholesky
                for (std::size_t j = 0; j < n; ++j)
                    u[static_cast<Eigen::Index>(j)] = normal(rng);
                z.noalias() = L * u;

                // Evolve all assets (even dead ones — cheaper than filtering)
                const Real dt = step_specs[i].dt;
                const Real sqrt_dt = step_specs[i].sqrt_dt;
                for (std::size_t j = 0; j < n; ++j)
                {
                    S[j] *= std::exp(asset_specs[j].base_drift * dt +
                                     asset_specs[j].vol * sqrt_dt *
                                         z[static_cast<Eigen::Index>(j)]);
                }

                // Find best return among alive assets
                Real best_return = -1e30;
                std::size_t best_idx = 0;
                for (std::size_t j = 0; j < n; ++j)
                {
                    if (!alive[j])
                        continue;
                    const Real ret = S[j] / m.spots[j] - 1.0;
                    if (ret > best_return)
                    {
                        best_return = ret;
                        best_idx = j;
                    }
                }

                sum_perf += best_return;
                alive[best_idx] = false;
            }

            // Payoff on average of locked-in returns
            const Real avg_perf = sum_perf / static_cast<Real>(n_obs);
            Real payoff;
            if (opt.is_call)
                payoff = std::max(avg_perf - opt.strike, 0.0);
            else
                payoff = std::max(opt.strike - avg_perf, 0.0);

            const Real disc_payoff = opt.notional * payoff * df;
            sum += disc_payoff;
            sum2 += disc_payoff * disc_payoff;
        }

        const auto N = static_cast<Real>(n_paths);
        const Real mean = sum / N;
        const Real var = sum2 / N - mean * mean;

        PricingResult out;
        out.npv = mean;
        out.mc_std_error = std::sqrt(var / N);
        out.diagnostics = "BSMountainMCEngine (" + m.model_name() +
                          ", paths=" + std::to_string(n_paths) +
                          ", assets=" + std::to_string(n) +
                          ", obs=" + std::to_string(n_obs) + ")";
        res_ = out;
    }

} // namespace quantModeling
