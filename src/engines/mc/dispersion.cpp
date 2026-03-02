#include "quantModeling/engines/mc/dispersion.hpp"

#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <Eigen/Core>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace quantModeling
{

    void DispersionMCEngine::visit(const DispersionSwap &ds)
    {
        const auto &m = require_model<MultiAssetBSModel>("DispersionMCEngine");
        const PricingSettings &settings = ctx_.settings;

        const auto n_assets = static_cast<std::size_t>(m.n_assets());
        if (ds.weights.size() != n_assets)
            throw InvalidInput("DispersionSwap: weights.size() != n_assets");
        if (ds.maturity <= 0.0)
            throw InvalidInput("DispersionSwap: maturity must be > 0");

        const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed = static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 42);

        // Build observation schedule
        std::vector<Time> sched;
        if (!ds.observation_dates.empty())
        {
            sched = ds.observation_dates;
        }
        else
        {
            const int n_obs = std::max(1, static_cast<int>(252.0 * ds.maturity));
            sched.resize(static_cast<std::size_t>(n_obs));
            for (int i = 0; i < n_obs; ++i)
                sched[static_cast<std::size_t>(i)] =
                    ds.maturity * static_cast<Real>(i + 1) / static_cast<Real>(n_obs);
        }
        const auto n_obs = sched.size();
        const Real T = ds.maturity;
        const Real r = m.rate_r;
        const Real df = m.discount_curve().discount(T);

        // Precompute per-step drift and vol per asset
        struct StepSpec
        {
            Real dt;
            std::vector<Real> drift;    // per asset
            std::vector<Real> vol_sqrt; // σ_i √dt
        };
        std::vector<StepSpec> steps(n_obs);
        {
            Time t_prev = 0.0;
            for (std::size_t k = 0; k < n_obs; ++k)
            {
                const Real dt = sched[k] - t_prev;
                steps[k].dt = dt;
                steps[k].drift.resize(n_assets);
                steps[k].vol_sqrt.resize(n_assets);
                for (std::size_t i = 0; i < n_assets; ++i)
                {
                    const Real sig = m.vols[i];
                    steps[k].drift[i] = (r - m.dividends[i] - 0.5 * sig * sig) * dt;
                    steps[k].vol_sqrt[i] = sig * std::sqrt(dt);
                }
                t_prev = sched[k];
            }
        }

        const Eigen::MatrixXd &L = m.chol; // lower-triangular Cholesky

        RngFactory rng_fact(seed);
        Real sum = 0.0, sum2 = 0.0;

        Eigen::VectorXd u(static_cast<Eigen::Index>(n_assets));
        Eigen::VectorXd corr_z(static_cast<Eigen::Index>(n_assets));

        for (int p = 0; p < n_paths; ++p)
        {
            Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
            NormalBoxMuller normal;

            // current spot per asset
            std::vector<Real> S(n_assets);
            for (std::size_t i = 0; i < n_assets; ++i)
                S[i] = m.spots[i];

            // index level history for log-returns
            std::vector<Real> idx_prev(1, 0.0);
            Real idx_0 = 0.0;
            for (std::size_t i = 0; i < n_assets; ++i)
                idx_0 += ds.weights[i] * S[i];

            // sum of squared log-returns per asset
            std::vector<Real> sum_lr2(n_assets, 0.0);
            Real sum_idx_lr2 = 0.0;

            Real prev_idx = idx_0;

            for (std::size_t k = 0; k < n_obs; ++k)
            {
                // Draw i.i.d. normals and correlate
                for (std::size_t i = 0; i < n_assets; ++i)
                    u(static_cast<Eigen::Index>(i)) = normal(rng);
                corr_z.noalias() = L * u;

                // Evolve each asset
                Real new_idx = 0.0;
                for (std::size_t i = 0; i < n_assets; ++i)
                {
                    const Real log_ret = steps[k].drift[i] +
                                         steps[k].vol_sqrt[i] * corr_z(static_cast<Eigen::Index>(i));
                    S[i] *= std::exp(log_ret);
                    sum_lr2[i] += log_ret * log_ret;
                    new_idx += ds.weights[i] * S[i];
                }

                // Index log-return
                const Real idx_lr = std::log(new_idx / prev_idx);
                sum_idx_lr2 += idx_lr * idx_lr;
                prev_idx = new_idx;
            }

            // Realised variances
            Real weighted_var = 0.0;
            for (std::size_t i = 0; i < n_assets; ++i)
                weighted_var += ds.weights[i] * (sum_lr2[i] / T);
            const Real idx_var = sum_idx_lr2 / T;

            const Real pv = ds.notional * (weighted_var - idx_var - ds.strike_spread) * df;
            sum += pv;
            sum2 += pv * pv;
        }

        const auto N = static_cast<Real>(n_paths);
        const Real mean = sum / N;
        const Real var = sum2 / N - mean * mean;

        PricingResult out;
        out.npv = mean;
        out.mc_std_error = std::sqrt(var / N);
        out.diagnostics = "DispersionMCEngine (paths=" + std::to_string(n_paths) +
                          ", assets=" + std::to_string(n_assets) +
                          ", obs=" + std::to_string(n_obs) + ")";
        res_ = out;
    }

    void DispersionMCEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void DispersionMCEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void DispersionMCEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void DispersionMCEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void DispersionMCEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void DispersionMCEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void DispersionMCEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
