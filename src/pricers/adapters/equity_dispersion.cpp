#include "quantModeling/pricers/adapters/equity_dispersion.hpp"

#include "quantModeling/engines/mc/dispersion.hpp"
#include "quantModeling/instruments/equity/dispersion.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Core>

namespace quantModeling
{

    PricingResult price_dispersion_bs_mc(const DispersionBSInput &in)
    {
        const auto n = in.spots.size();
        if (n < 2)
            throw InvalidInput("DispersionBSInput: need at least 2 assets");
        if (in.vols.size() != n || in.dividends.size() != n || in.weights.size() != n)
            throw InvalidInput("DispersionBSInput: dimension mismatch");

        // Build correlation matrix
        Eigen::MatrixXd corr = Eigen::MatrixXd::Identity(
            static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        if (!in.correlations.empty())
        {
            if (in.correlations.size() != n)
                throw InvalidInput("DispersionBSInput: correlations dimension mismatch");
            for (std::size_t i = 0; i < n; ++i)
            {
                if (in.correlations[i].size() != n)
                    throw InvalidInput("DispersionBSInput: correlations not square");
                for (std::size_t j = 0; j < n; ++j)
                    corr(static_cast<Eigen::Index>(i),
                         static_cast<Eigen::Index>(j)) = in.correlations[i][j];
            }
        }

        auto model = std::make_shared<MultiAssetBSModel>(
            in.rate, in.spots, in.vols, in.dividends, corr);

        DispersionSwap ds(in.maturity, in.strike_spread, in.weights,
                          in.notional, in.observation_dates);

        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        PricingContext ctx{MarketView{}, settings, model};
        DispersionMCEngine engine(ctx);
        return price(ds, engine);
    }

} // namespace quantModeling
