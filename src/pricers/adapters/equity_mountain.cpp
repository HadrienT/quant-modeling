#include "quantModeling/pricers/adapters/equity_mountain.hpp"

#include "quantModeling/engines/mc/mountain.hpp"
#include "quantModeling/instruments/equity/mountain.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Core>
#include <memory>

namespace quantModeling
{

    PricingResult price_equity_mountain_bs_mc(const MountainBSInput &in)
    {
        // ── Validate dimensions ───────────────────────────────────────
        const auto n = static_cast<int>(in.spots.size());
        if (n < 2)
            throw InvalidInput("MountainBSInput: need at least 2 assets");
        if (static_cast<int>(in.vols.size()) != n)
            throw InvalidInput("MountainBSInput: vols.size() != spots.size()");
        if (static_cast<int>(in.dividends.size()) != n)
            throw InvalidInput("MountainBSInput: dividends.size() != spots.size()");
        if (static_cast<int>(in.observation_dates.size()) != n)
            throw InvalidInput("MountainBSInput: observation_dates.size() must equal n_assets");

        // ── Build correlation matrix ──────────────────────────────────
        Eigen::MatrixXd corr(n, n);
        if (in.correlations.empty())
        {
            corr.setIdentity();
        }
        else
        {
            if (static_cast<int>(in.correlations.size()) != n)
                throw InvalidInput("MountainBSInput: correlations must be n×n or empty");
            for (int i = 0; i < n; ++i)
            {
                if (static_cast<int>(in.correlations[i].size()) != n)
                    throw InvalidInput("MountainBSInput: correlations row size mismatch");
                for (int j = 0; j < n; ++j)
                    corr(i, j) = in.correlations[i][j];
            }
        }

        // ── Build model ──────────────────────────────────────────────
        auto model = std::make_shared<MultiAssetBSModel>(
            in.rate, in.spots, in.vols, in.dividends, corr);

        // ── Build instrument ─────────────────────────────────────────
        MountainOption opt(in.observation_dates, in.strike,
                           in.is_call, in.notional);

        // ── Pricing context ──────────────────────────────────────────
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSMountainMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
