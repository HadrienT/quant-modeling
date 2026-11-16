#ifndef QM_ENGINES_MC_SCRIPT_ADJOINT_HPP
#define QM_ENGINES_MC_SCRIPT_ADJOINT_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/models/device_model.hpp"

/**
 * @file script_adjoint.hpp
 * @brief How many scalar risks the GPU's forward mode differentiates for a
 *        model (blueprint/wp/19-gpu.md §6, lot G3).
 *
 * The risks themselves come from the kernels generated from the script's IR
 * (blueprint/wp/27-ir-ad.md: the tangent for Black-Scholes and Heston, the
 * adjoint for the grid models). The dual numbers on the interpreter and the
 * local-vol adjoint written by hand, which came first and served as their
 * oracles, were deleted on 2026-11-16 once the generated kernels beat them
 * (§14.3, §14.6); the CPU tape stays the oracle of every derivative.
 */

namespace quantModeling::mc
{

    /// Parameters the tangent differentiates, in the CPU model's label order
    /// (0 when the model has no tangent on the device):
    ///   Black-Scholes, one asset:  spot, rate, div, vol
    ///   Black-Scholes, n assets:   rate, spot[i], div[i], vol[i]
    ///   Heston (BatesSimModel):    spot, rate, div, v0, kappa, theta, xi, rho
    ///                              (then the three jump labels, whose risks
    ///                              are zero with no jumps)
    QM_HOST_DEVICE inline int dual_directions(DeviceModel::Kind kind, int n_assets)
    {
        switch (kind)
        {
            case DeviceModel::Kind::BlackScholes:
                return n_assets == 1 ? 4 : 1 + 3 * n_assets;
            case DeviceModel::Kind::Heston:
                return 8;
            default:
                return 0;
        }
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SCRIPT_ADJOINT_HPP
