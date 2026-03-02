#include "quantModeling/pricers/adapters/equity_rainbow.hpp"

#include "quantModeling/engines/mc/rainbow.hpp"
#include "quantModeling/instruments/equity/rainbow.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Core>
#include <memory>

namespace quantModeling
{

    namespace
    {
        std::shared_ptr<MultiAssetBSModel> build_model(const RainbowBSInput &in)
        {
            const auto n = static_cast<int>(in.spots.size());
            if (n < 2)
                throw InvalidInput("RainbowBSInput: need at least 2 assets");
            if (static_cast<int>(in.vols.size()) != n)
                throw InvalidInput("RainbowBSInput: vols.size() != spots.size()");
            if (static_cast<int>(in.dividends.size()) != n)
                throw InvalidInput("RainbowBSInput: dividends.size() != spots.size()");

            Eigen::MatrixXd corr(n, n);
            if (in.correlations.empty())
            {
                corr.setIdentity();
            }
            else
            {
                if (static_cast<int>(in.correlations.size()) != n)
                    throw InvalidInput("RainbowBSInput: correlations must be n×n or empty");
                for (int i = 0; i < n; ++i)
                {
                    if (static_cast<int>(in.correlations[i].size()) != n)
                        throw InvalidInput("RainbowBSInput: correlations row size mismatch");
                    for (int j = 0; j < n; ++j)
                        corr(i, j) = in.correlations[i][j];
                }
            }

            return std::make_shared<MultiAssetBSModel>(
                in.rate, in.spots, in.vols, in.dividends, corr);
        }
    } // anonymous namespace

    PricingResult price_worst_of_bs_mc(const RainbowBSInput &in)
    {
        auto model = build_model(in);
        WorstOfOption opt(in.maturity, in.strike, in.is_call, in.notional);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        PricingContext ctx{MarketView{}, settings, model};
        RainbowMCEngine engine(ctx);
        return price(opt, engine);
    }

    PricingResult price_best_of_bs_mc(const RainbowBSInput &in)
    {
        auto model = build_model(in);
        BestOfOption opt(in.maturity, in.strike, in.is_call, in.notional);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        PricingContext ctx{MarketView{}, settings, model};
        RainbowMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
