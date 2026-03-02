#include "quantModeling/engines/mc/rainbow.hpp"

#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace quantModeling
{

    // ─── helpers ────────────────────────────────────────────────────────
    namespace
    {
        enum class RainbowType
        {
            WorstOf,
            BestOf
        };

        PricingResult price_rainbow(const MultiAssetBSModel &m,
                                    const PricingSettings &settings,
                                    Time maturity, Real strike, bool is_call,
                                    Real notional, RainbowType rainbow)
        {
            const auto n = static_cast<std::size_t>(m.n_assets());
            if (n < 2)
                throw InvalidInput("Rainbow option: need at least 2 assets");
            if (maturity <= 0.0)
                throw InvalidInput("Rainbow option: maturity must be > 0");
            if (notional == 0.0)
                throw InvalidInput("Rainbow option: notional must be non-zero");

            const int n_paths = settings.mc_paths > 0 ? settings.mc_paths : 100000;
            const auto seed = static_cast<uint64_t>(
                settings.mc_seed > 0 ? settings.mc_seed : 42);

            const Real r = m.rate_r;
            const Real T = maturity;
            const Real df = m.discount_curve().discount(T);

            // Pre-compute drift and vol for each asset for time T
            struct AssetSpec
            {
                Real drift; // (r − q − 0.5σ²) × T
                Real vol;   // σ × √T
                Real S0;    // initial spot
            };
            std::vector<AssetSpec> specs(n);
            const Real sqrt_T = std::sqrt(T);
            for (std::size_t i = 0; i < n; ++i)
            {
                const Real sig = m.vols[i];
                specs[i].drift = (r - m.dividends[i] - 0.5 * sig * sig) * T;
                specs[i].vol = sig * sqrt_T;
                specs[i].S0 = m.spots[i];
            }

            const Eigen::MatrixXd &L = m.chol;

            RngFactory rng_fact(seed);
            Real sum = 0.0, sum2 = 0.0;

            Eigen::VectorXd u(static_cast<Eigen::Index>(n));
            Eigen::VectorXd z(static_cast<Eigen::Index>(n));

            for (int p = 0; p < n_paths; ++p)
            {
                Pcg32 rng = rng_fact.make(static_cast<uint64_t>(p));
                NormalBoxMuller normal;

                // Draw correlated normals
                for (std::size_t i = 0; i < n; ++i)
                    u(static_cast<Eigen::Index>(i)) = normal(rng);
                z.noalias() = L * u;

                // Compute terminal performances
                Real extremal_perf;
                if (rainbow == RainbowType::WorstOf)
                    extremal_perf = std::numeric_limits<Real>::max();
                else
                    extremal_perf = -std::numeric_limits<Real>::max();

                for (std::size_t i = 0; i < n; ++i)
                {
                    const Real log_ret = specs[i].drift +
                                         specs[i].vol * z(static_cast<Eigen::Index>(i));
                    const Real S_T = specs[i].S0 * std::exp(log_ret);
                    const Real perf = S_T / specs[i].S0; // performance ratio

                    if (rainbow == RainbowType::WorstOf)
                        extremal_perf = std::min(extremal_perf, perf);
                    else
                        extremal_perf = std::max(extremal_perf, perf);
                }

                // Payoff
                Real payoff;
                if (is_call)
                    payoff = std::max(extremal_perf - strike, 0.0);
                else
                    payoff = std::max(strike - extremal_perf, 0.0);

                const Real disc_payoff = notional * payoff * df;
                sum += disc_payoff;
                sum2 += disc_payoff * disc_payoff;
            }

            const auto N = static_cast<Real>(n_paths);
            const Real mean = sum / N;
            const Real var = sum2 / N - mean * mean;

            PricingResult out;
            out.npv = mean;
            out.mc_std_error = std::sqrt(var / N);

            const char *type_str = (rainbow == RainbowType::WorstOf)
                                       ? "WorstOf"
                                       : "BestOf";
            out.diagnostics = std::string("RainbowMCEngine:") + type_str +
                              " (paths=" + std::to_string(n_paths) +
                              ", assets=" + std::to_string(n) + ")";
            return out;
        }
    } // anonymous namespace

    // ─── Worst-of visit ─────────────────────────────────────────────────

    void RainbowMCEngine::visit(const WorstOfOption &opt)
    {
        const auto &m = require_model<MultiAssetBSModel>("RainbowMCEngine");
        res_ = price_rainbow(m, ctx_.settings, opt.maturity, opt.strike,
                             opt.is_call, opt.notional, RainbowType::WorstOf);
    }

    // ─── Best-of visit ──────────────────────────────────────────────────

    void RainbowMCEngine::visit(const BestOfOption &opt)
    {
        const auto &m = require_model<MultiAssetBSModel>("RainbowMCEngine");
        res_ = price_rainbow(m, ctx_.settings, opt.maturity, opt.strike,
                             opt.is_call, opt.notional, RainbowType::BestOf);
    }

    // ─── rejections ─────────────────────────────────────────────────────

    void RainbowMCEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void RainbowMCEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void RainbowMCEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void RainbowMCEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void RainbowMCEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void RainbowMCEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void RainbowMCEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
