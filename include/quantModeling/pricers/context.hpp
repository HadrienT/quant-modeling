#ifndef PRICERS_CONTEXT_HPP
#define PRICERS_CONTEXT_HPP

#include <memory>

#include "quantModeling/models/base.hpp"
#include "quantModeling/market/discount_curve.hpp"

namespace quantModeling
{

  struct DiscountCurve;
  struct VolSurface;
  struct Fixings;

  /// How N(0,1) samples are produced by MC engines.
  /// BoxMuller reproduces the historical draw sequence; InverseNormal is the
  /// stateless transform required by QMC (Sobol) and the CUDA backend.
  enum class GaussianKind
  {
    BoxMuller,
    InverseNormal
  };

  struct PricingSettings
  {
    int mc_paths = 0;
    int mc_seed = 0;
    bool mc_antithetic = true;
    int tree_steps = 0;
    int pde_space_steps = 0;
    int pde_time_steps = 0;
    // Last field: keeps aggregate initialization of the fields above intact.
    GaussianKind mc_gaussian = GaussianKind::BoxMuller;
  };

  struct MarketView
  {
    std::shared_ptr<const DiscountCurve> discount;
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