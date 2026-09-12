#ifndef PRICERS_CONTEXT_HPP
#define PRICERS_CONTEXT_HPP

#include <memory>

#include "quantModeling/core/results.hpp"
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

    /// Point-set driving the Monte-Carlo paths.
    /// PseudoRandom: PCG32 stream. Sobol: scrambled low-discrepancy sequence
    /// (randomized QMC — paths are split into mc_rqmc_batches independent
    /// digital shifts; the reported std error is the spread of batch means).
    /// Stratified: jittered equiprobable strata of the first uniform — same
    /// batching scheme as Sobol for the error bars.
    enum class SamplerKind
    {
        PseudoRandom,
        Sobol,
        Stratified
    };

    struct PricingSettings
    {
        int mc_paths = 0;
        int mc_seed = 0;
        bool mc_antithetic = true;
        int tree_steps = 0;
        int pde_space_steps = 0;
        int pde_time_steps = 0;
        // New fields last: keeps aggregate initialization of the fields above intact.
        GaussianKind mc_gaussian = GaussianKind::BoxMuller;
        SamplerKind mc_sampler = SamplerKind::PseudoRandom;
        int mc_rqmc_batches = 16;            ///< RQMC replicates when mc_sampler == Sobol
        bool mc_control_variate = true;      ///< use analytic controls where available
        bool mc_importance_sampling = false; ///< drift-shift IS (OTM vanillas)
        bool mc_cmc = false;                 ///< conditional MC: smooth indicators analytically
        bool mc_bridge_extrema = false;      ///< sample continuous extrema via bridge inverse transform
        /// Real (not just declared) for a product on the timeline
        /// architecture with T = aad::Number available -- see
        /// engines/mc/simulation_engine_aad.hpp. A caller still picks which
        /// top-level function to invoke (simulate vs simulate_aad) and which
        /// T to instantiate the product/model with; this flag is what the
        /// orchestration layer (a router, a future generic price()) reads
        /// to make that choice, rather than a raw string compared ad hoc at
        /// every call site.
        GreeksMethod greeks = GreeksMethod::Bump;
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