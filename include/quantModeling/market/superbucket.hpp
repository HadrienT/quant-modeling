#ifndef MARKET_SUPERBUCKET_HPP
#define MARKET_SUPERBUCKET_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/svi_calibration.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace quantModeling
{

    /// The sensitivity of a price to one quoted implied vol.
    struct QuoteVega
    {
        std::size_t slice = 0;
        Real ttm = 0.0;
        Real log_moneyness = 0.0; ///< k = ln(K / F_T), as the slice was fitted
        Real strike = 0.0;        ///< F_T e^k
        Real implied_vol = 0.0;   ///< the quote
        Real vega = 0.0;          ///< dV / d(implied vol), per unit of vol
    };

    struct SuperbucketResult
    {
        std::vector<QuoteVega> quotes;
        /// dV / d(a, b, rho, m, sigma) of each SVI slice.
        std::vector<std::array<Real, 5>> dV_dsvi;
        /// Parameters held at a bound of their calibration: frozen, so they
        /// carry no sensitivity to the quotes.
        std::vector<std::array<bool, 5>> at_bound;
    };

    /**
     * @brief Market risk of a local-vol price: dV / d(each quoted implied vol)
     *        -- the Dupire "superbucket" of Savine's book
     *        (blueprint/wp/17-aad.md §11, lot 17h).
     *
     * The simulation's AAD gives model risks, dV/dsigma_loc on the Dupire grid
     * (`dV_dsigma_loc`, K-major, the grid build_local_vol_grid makes from
     * these slices). Two steps take them to the quotes:
     *
     * 1. Through Dupire, an explicit function of the SVI parameters: the
     *    grid is rebuilt on an AAD tape (the same cells, the same gap fill,
     *    the same clamps as the double build), its outputs seeded with
     *    dV/dsigma_loc, and one reverse pass gives dV/dtheta for every slice
     *    -- the book's check-pointing through the calibration.
     * 2. Through each slice's SVI fit, an iterative calibration: by the
     *    implicit function theorem at the optimum of sum w_i r_i^2,
     *    r_i = iv(k_i; theta) - m_i, with the Gauss-Newton Hessian:
     *    dtheta/dm = (J' W J)^-1 J' W, J = dr/dtheta computed by AAD. So
     *    dV/dm_i = w_i J_i . (J' W J)^-1 dV/dtheta. Parameters at a bound
     *    of the fit are held fixed (their dtheta/dm is zero).
     *
     * The Gauss-Newton Hessian drops sum w_i r_i d2r_i/dtheta2: exact on a
     * perfect fit, first order in the fit residuals otherwise (ADR-A9).
     */
    SuperbucketResult dupire_superbucket(
        const std::vector<SVISliceCalibration> &slices,
        const std::vector<std::vector<SVISliceQuote>> &quotes,
        Real spot, Real rate, Real dividend, Real k_min, Real k_max,
        std::size_t n_strikes, std::size_t n_maturities,
        const std::vector<Real> &dV_dsigma_loc,
        const DupireFromSVIParams &params = {});

} // namespace quantModeling

#endif
