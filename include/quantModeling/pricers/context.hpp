#ifndef PRICERS_CONTEXT_HPP
#define PRICERS_CONTEXT_HPP

#include <memory>

#include "quantModeling/models/base.hpp"

namespace quantModeling
{

    struct DiscountCurve;
    struct VolSurface;
    struct Fixings;

    struct PricingSettings
    {
        int mc_paths = 0;
        int mc_seed = 0;
        double fd_tol = 0.0;
    };

    struct MarketView
    {
        // std::shared_ptr<const DiscountCurve> discount;
        // std::shared_ptr<const VolSurface>   vol;
        // std::shared_ptr<const Fixings>      fixings;
    };

    struct PricingContext
    {
        MarketView market;
        PricingSettings settings;
        std::shared_ptr<const IModel> model;
    };

} // namespace quantModeling

#endif