#include "quantModeling/pricers/adapters/equity_basket.hpp"

#include "quantModeling/engines/mc/basket.hpp"
#include "quantModeling/instruments/equity/basket.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Core>
#include <memory>
#include <stdexcept>

namespace quantModeling
{

    PricingResult price_equity_basket_bs_mc(const BasketBSInput &in)
    {
        // ── Validate dimensions ───────────────────────────────────────
        const int n = static_cast<int>(in.spots.size());
        if (n < 2)
            throw InvalidInput("BasketBSInput: need at least 2 assets");
        if (static_cast<int>(in.vols.size()) != n)
            throw InvalidInput("BasketBSInput: vols.size() != spots.size()");
        if (static_cast<int>(in.dividends.size()) != n)
            throw InvalidInput("BasketBSInput: dividends.size() != spots.size()");
        if (static_cast<int>(in.weights.size()) != n)
            throw InvalidInput("BasketBSInput: weights.size() != spots.size()");

        // ── Build correlation matrix ──────────────────────────────────
        // correlations may be:
        //   - n×n full matrix (in.correlations.size() == n)
        //   - empty → identity (zero pairwise correlation)
        Eigen::MatrixXd corr(n, n);
        if (in.correlations.empty())
        {
            corr.setIdentity();
        }
        else
        {
            if (static_cast<int>(in.correlations.size()) != n)
                throw InvalidInput("BasketBSInput: correlations must be n×n or empty");
            for (int i = 0; i < n; ++i)
            {
                if (static_cast<int>(in.correlations[i].size()) != n)
                    throw InvalidInput("BasketBSInput: correlations row size mismatch");
                for (int j = 0; j < n; ++j)
                    corr(i, j) = in.correlations[i][j];
            }
        }

        // ── Build model (Cholesky inside constructor) ─────────────────
        auto model = std::make_shared<MultiAssetBSModel>(
            static_cast<Real>(in.rate),
            in.spots, in.vols, in.dividends,
            corr);

        // ── Build instrument ──────────────────────────────────────────
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        BasketOption opt(payoff, exercise, in.weights, 1.0);

        // ── Pricing context ───────────────────────────────────────────
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        settings.mc_antithetic = in.mc_antithetic;

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSBasketMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
