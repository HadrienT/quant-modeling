#ifndef UTILS_DUAL_HPP
#define UTILS_DUAL_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"

/**
 * @file dual.hpp
 * @brief Forward-mode dual numbers, host and device
 *        (blueprint/wp/19-gpu.md §6, lot G3, ADR-G4).
 *
 * A Dual<N> carries a value and its N partial derivatives with respect to
 * N chosen inputs (a model's parameters). Every operation propagates them by
 * the chain rule as it goes: no tape, no memory beyond the N + 1 doubles,
 * which is why it runs on the device where a tape per thread would not fit.
 * The cost is proportional to N, so it is the GPU's AAD for models with few
 * parameters (Black-Scholes, Heston); the local vol's thousands of grid
 * points take the per-path adjoint instead (engines/mc/script_adjoint.hpp).
 *
 * The local derivatives, and the choice made at a kink, are those of the CPU
 * tape (aad/expression.hpp's operation table): max(l, r) differentiates l
 * when l > r, min(l, r) when l < r, fabs(x) takes +1 at 0, pow(l, r)'s
 * right derivative is v log l. With the same draws, a dual run therefore
 * reproduces the CPU adjoint path by path, up to rounding.
 *
 * Comparisons look at the value only, as the tape's do: control flow is not
 * differentiated.
 */

namespace quantModeling
{
    // Its own namespace: the functions below are found by argument-dependent
    // lookup on a Dual, and hide nothing for doubles elsewhere.
    namespace dual
    {

        template <int N>
        struct Dual
        {
            double v = 0.0;
            double d[N] = {};

            QM_HOST_DEVICE Dual() {}
            QM_HOST_DEVICE Dual(double x)
                : v(x) {} // NOLINT: implicit, like a double constant on the tape

            /// The input number i: value x, derivative 1 in direction i.
            QM_HOST_DEVICE static Dual variable(double x, int i)
            {
                Dual r(x);
                r.d[i] = 1.0;
                return r;
            }

            QM_HOST_DEVICE double value() const { return v; }
            QM_HOST_DEVICE explicit operator double() const { return v; }

            QM_HOST_DEVICE Dual &operator+=(const Dual &o)
            {
                v += o.v;
                for (int i = 0; i < N; ++i)
                    d[i] += o.d[i];
                return *this;
            }
            QM_HOST_DEVICE Dual &operator-=(const Dual &o)
            {
                v -= o.v;
                for (int i = 0; i < N; ++i)
                    d[i] -= o.d[i];
                return *this;
            }
            QM_HOST_DEVICE Dual &operator*=(const Dual &o)
            {
                for (int i = 0; i < N; ++i)
                    d[i] = d[i] * o.v + v * o.d[i];
                v *= o.v;
                return *this;
            }
            QM_HOST_DEVICE Dual &operator/=(const Dual &o)
            {
                const double q = v / o.v;
                for (int i = 0; i < N; ++i)
                    d[i] = d[i] / o.v - q / o.v * o.d[i];
                v = q;
                return *this;
            }
        };

        /// A value as a double, for code templated on its number type.
        template <int N>
        QM_HOST_DEVICE double value_of(const Dual<N> &x)
        {
            return x.v;
        }

        namespace dual_detail
        {
            /// df * dx, but 0 when x does not depend on direction i: the tape
            /// never sends an adjoint into a constant, so an infinite or NaN
            /// local derivative there (sqrt at 0, pow of a negative base) must
            /// not turn 0 into NaN.
            QM_HOST_DEVICE inline double times(double df, double dx)
            {
                return dx == 0.0 ? 0.0 : df * dx;
            }

            /// f(x) with f'(x) = df: the chain rule for a unary function.
            template <int N>
            QM_HOST_DEVICE Dual<N> chain(const Dual<N> &x, double f, double df)
            {
                Dual<N> r(f);
                for (int i = 0; i < N; ++i)
                    r.d[i] = times(df, x.d[i]);
                return r;
            }
        } // namespace dual_detail

        // ── arithmetic, dual (op) dual and mixed with double ───────────────────
        template <int N>
        QM_HOST_DEVICE Dual<N> operator+(Dual<N> a, const Dual<N> &b)
        {
            return a += b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator-(Dual<N> a, const Dual<N> &b)
        {
            return a -= b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator*(Dual<N> a, const Dual<N> &b)
        {
            return a *= b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator/(Dual<N> a, const Dual<N> &b)
        {
            return a /= b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator+(Dual<N> a, double b)
        {
            a.v += b;
            return a;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator+(double a, Dual<N> b)
        {
            b.v += a;
            return b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator-(Dual<N> a, double b)
        {
            a.v -= b;
            return a;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator-(double a, const Dual<N> &b)
        {
            Dual<N> r(a - b.v);
            for (int i = 0; i < N; ++i)
                r.d[i] = -b.d[i];
            return r;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator*(Dual<N> a, double b)
        {
            a.v *= b;
            for (int i = 0; i < N; ++i)
                a.d[i] *= b;
            return a;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator*(double a, Dual<N> b)
        {
            return b * a;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator/(Dual<N> a, double b)
        {
            a.v /= b;
            for (int i = 0; i < N; ++i)
                a.d[i] /= b;
            return a;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator/(double a, const Dual<N> &b)
        {
            return Dual<N>(a) / b;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator-(const Dual<N> &a)
        {
            return dual_detail::chain(a, -a.v, -1.0);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> operator+(const Dual<N> &a)
        {
            return a;
        }

        // ── comparisons: on the value ──────────────────────────────────────────
#define QM_DUAL_COMPARISON(OP)                                          \
    template <int N>                                                    \
    QM_HOST_DEVICE bool operator OP(const Dual<N> &a, const Dual<N> &b) \
    {                                                                   \
        return a.v OP b.v;                                              \
    }                                                                   \
    template <int N>                                                    \
    QM_HOST_DEVICE bool operator OP(const Dual<N> &a, double b)         \
    {                                                                   \
        return a.v OP b;                                                \
    }                                                                   \
    template <int N>                                                    \
    QM_HOST_DEVICE bool operator OP(double a, const Dual<N> &b)         \
    {                                                                   \
        return a OP b.v;                                                \
    }
        QM_DUAL_COMPARISON(==)
        QM_DUAL_COMPARISON(!=)
        QM_DUAL_COMPARISON(<)
        QM_DUAL_COMPARISON(<=)
        QM_DUAL_COMPARISON(>)
        QM_DUAL_COMPARISON(>=)
#undef QM_DUAL_COMPARISON

        // ── functions (found by argument-dependent lookup after `using std::…`) ─
        template <int N>
        QM_HOST_DEVICE Dual<N> exp(const Dual<N> &x)
        {
            const double e = std::exp(x.v);
            return dual_detail::chain(x, e, e);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> log(const Dual<N> &x)
        {
            return dual_detail::chain(x, std::log(x.v), 1.0 / x.v);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> sqrt(const Dual<N> &x)
        {
            const double s = std::sqrt(x.v);
            return dual_detail::chain(x, s, 0.5 / s);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> fabs(const Dual<N> &x)
        {
            return dual_detail::chain(x, std::fabs(x.v), x.v >= 0.0 ? 1.0 : -1.0);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> pow(const Dual<N> &l, const Dual<N> &r)
        {
            const double p = std::pow(l.v, r.v);
            const double dl = r.v * p / l.v;
            const double dr = p * std::log(l.v);
            Dual<N> out(p);
            for (int i = 0; i < N; ++i)
                out.d[i] = dual_detail::times(dl, l.d[i]) + dual_detail::times(dr, r.d[i]);
            return out;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> max(const Dual<N> &l, const Dual<N> &r)
        {
            return l.v > r.v ? l : r;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> max(const Dual<N> &l, double r)
        {
            return l.v > r ? l : Dual<N>(r);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> max(double l, const Dual<N> &r)
        {
            return l > r.v ? Dual<N>(l) : r;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> min(const Dual<N> &l, const Dual<N> &r)
        {
            return l.v < r.v ? l : r;
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> min(const Dual<N> &l, double r)
        {
            return l.v < r ? l : Dual<N>(r);
        }
        template <int N>
        QM_HOST_DEVICE Dual<N> min(double l, const Dual<N> &r)
        {
            return l < r.v ? Dual<N>(l) : r;
        }

    } // namespace dual

    using dual::Dual;

} // namespace quantModeling

#endif // UTILS_DUAL_HPP
