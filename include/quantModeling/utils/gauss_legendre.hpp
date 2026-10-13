#ifndef QM_UTILS_GAUSS_LEGENDRE_HPP
#define QM_UTILS_GAUSS_LEGENDRE_HPP

#include "quantModeling/core/types.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace quantModeling
{

    /// Gauss-Legendre nodes and weights on [-1, 1], sixteen of them, by
    /// Newton on the Legendre polynomial. They integrate a product of
    /// exponentials over one step of an exposure grid to the last digits.
    struct GaussLegendre16
    {
        static constexpr int kNodes = 16;
        std::array<Real, kNodes> node, weight;

        GaussLegendre16()
        {
            for (int i = 0; i < kNodes; ++i)
            {
                Real x = std::cos(std::numbers::pi * (i + 0.75) / (kNodes + 0.5));
                Real derivative = 0.0;
                for (int iteration = 0; iteration < 100; ++iteration)
                {
                    Real p0 = 1.0, p1 = x;
                    for (int k = 2; k <= kNodes; ++k)
                    {
                        const Real p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
                        p0 = p1;
                        p1 = p2;
                    }
                    derivative = kNodes * (x * p1 - p0) / (x * x - 1.0);
                    const Real step = p1 / derivative;
                    x -= step;
                    if (std::abs(step) < 1e-16)
                        break;
                }
                node[i] = x;
                weight[i] = 2.0 / ((1.0 - x * x) * derivative * derivative);
            }
        }
    };

    /**
     * @brief ∫ f over [from, to] with sixteen Gauss-Legendre nodes. `f` may
     *        return a double or a differentiable number (it must then return
     *        the number itself, not an expression of it): the integral is of
     *        the same type, so that it can be recorded on a tape.
     */
    template <class F>
    auto gauss_legendre(Time from, Time to, F f)
    {
        static const GaussLegendre16 rule;
        const Real half = 0.5 * (to - from), middle = 0.5 * (to + from);
        using Result = std::decay_t<decltype(f(middle))>;
        Result sum(0.0);
        for (int i = 0; i < GaussLegendre16::kNodes; ++i)
            sum = sum + rule.weight[i] * f(middle + half * rule.node[i]);
        return Result(half * sum);
    }

} // namespace quantModeling

#endif // QM_UTILS_GAUSS_LEGENDRE_HPP
