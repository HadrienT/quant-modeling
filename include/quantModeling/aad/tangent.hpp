#ifndef QM_AAD_TANGENT_HPP
#define QM_AAD_TANGENT_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>

namespace quantModeling::aad
{

    /**
     * @brief A forward-mode tangent over any scalar S: a value and its
     *        derivative in one direction, both of type S (blueprint/wp/17-aad.md
     *        §18, second order).
     *
     * With S = double it is the textbook dual number. With S = Number it is
     * the tangent-linear model *recorded on the tape*: the pathwise first
     * derivative d (a delta) is itself a Number, and one adjoint pass from it
     * returns its gradient -- a whole row of the Hessian (gamma, vanna,
     * d delta / d rate...) for the cost of one more adjoint sweep, with no
     * bump anywhere. That is "adjoint over tangent" (Griewank & Walther 2008,
     * ch. 5; Naumann 2012, ch. 3), the standard second-order mode.
     *
     * Every model and product of the timeline architecture is templated on
     * its number type, so they instantiate on Tangent<Number> unchanged: the
     * operations below compute the value and the tangent with S's own
     * operations (the tape records both), and control flow reads values only,
     * like Number's comparisons.
     *
     * At a kink the derivative is the one the tape takes (max picks the
     * larger side, fabs takes +1 at 0), so the second derivative of a hard
     * payoff is zero almost everywhere: gammas of digitals and barriers need
     * the fuzzy evaluator's smoothing, as their first-order AAD deltas do.
     */
    template <class S>
    struct Tangent
    {
        S v{};
        S d{};

        Tangent() = default;
        Tangent(double x)
            : v(x), d(0.0) {} // NOLINT: a constant, like Number(double) in expressions
        Tangent(S value, S derivative)
            : v(std::move(value)), d(std::move(derivative)) {}

        explicit operator double() const { return static_cast<double>(v); }

        Tangent &operator+=(const Tangent &o) { return *this = *this + o; }
        Tangent &operator-=(const Tangent &o) { return *this = *this - o; }
        Tangent &operator*=(const Tangent &o) { return *this = *this * o; }
        Tangent &operator/=(const Tangent &o) { return *this = *this / o; }
    };

    namespace tangent_detail
    {
        /// Materialise an expression of S (an expression template when S is
        /// Number) into an S.
        template <class S, class E>
        S s(const E &e)
        {
            return S(e);
        }
        template <class S>
        double val(const Tangent<S> &x)
        {
            return static_cast<double>(x.v);
        }
    } // namespace tangent_detail

    // ── arithmetic ──────────────────────────────────────────────────────────

    template <class S>
    Tangent<S> operator+(const Tangent<S> &a, const Tangent<S> &b)
    {
        using tangent_detail::s;
        return {s<S>(a.v + b.v), s<S>(a.d + b.d)};
    }
    template <class S>
    Tangent<S> operator-(const Tangent<S> &a, const Tangent<S> &b)
    {
        using tangent_detail::s;
        return {s<S>(a.v - b.v), s<S>(a.d - b.d)};
    }
    template <class S>
    Tangent<S> operator-(const Tangent<S> &a)
    {
        using tangent_detail::s;
        return {s<S>(-a.v), s<S>(-a.d)};
    }
    template <class S>
    Tangent<S> operator*(const Tangent<S> &a, const Tangent<S> &b)
    {
        using tangent_detail::s;
        return {s<S>(a.v * b.v), s<S>(a.d * b.v + a.v * b.d)};
    }
    template <class S>
    Tangent<S> operator/(const Tangent<S> &a, const Tangent<S> &b)
    {
        using tangent_detail::s;
        const S q = s<S>(a.v / b.v);
        return {q, s<S>((a.d - q * b.d) / b.v)};
    }

    // Mixed with double: a constant has no tangent.
    template <class S>
    Tangent<S> operator+(const Tangent<S> &a, double b)
    {
        return {tangent_detail::s<S>(a.v + b), a.d};
    }
    template <class S>
    Tangent<S> operator+(double a, const Tangent<S> &b)
    {
        return {tangent_detail::s<S>(a + b.v), b.d};
    }
    template <class S>
    Tangent<S> operator-(const Tangent<S> &a, double b)
    {
        return {tangent_detail::s<S>(a.v - b), a.d};
    }
    template <class S>
    Tangent<S> operator-(double a, const Tangent<S> &b)
    {
        return {tangent_detail::s<S>(a - b.v), tangent_detail::s<S>(-b.d)};
    }
    template <class S>
    Tangent<S> operator*(const Tangent<S> &a, double b)
    {
        return {tangent_detail::s<S>(a.v * b), tangent_detail::s<S>(a.d * b)};
    }
    template <class S>
    Tangent<S> operator*(double a, const Tangent<S> &b)
    {
        return {tangent_detail::s<S>(a * b.v), tangent_detail::s<S>(a * b.d)};
    }
    template <class S>
    Tangent<S> operator/(const Tangent<S> &a, double b)
    {
        return {tangent_detail::s<S>(a.v / b), tangent_detail::s<S>(a.d / b)};
    }
    template <class S>
    Tangent<S> operator/(double a, const Tangent<S> &b)
    {
        using tangent_detail::s;
        const S q = s<S>(a / b.v);
        return {q, s<S>(-(q * b.d) / b.v)};
    }

    // ── functions ───────────────────────────────────────────────────────────

    template <class S>
    Tangent<S> exp(const Tangent<S> &x)
    {
        using std::exp;
        using tangent_detail::s;
        const S e = s<S>(exp(x.v));
        return {e, s<S>(e * x.d)};
    }
    template <class S>
    Tangent<S> log(const Tangent<S> &x)
    {
        using std::log;
        using tangent_detail::s;
        return {s<S>(log(x.v)), s<S>(x.d / x.v)};
    }
    template <class S>
    Tangent<S> sqrt(const Tangent<S> &x)
    {
        using std::sqrt;
        using tangent_detail::s;
        const S r = s<S>(sqrt(x.v));
        return {r, s<S>(x.d / (2.0 * r))};
    }
    template <class S>
    Tangent<S> fabs(const Tangent<S> &x)
    {
        using tangent_detail::s;
        return tangent_detail::val(x) >= 0.0 ? x : Tangent<S>{s<S>(-x.v), s<S>(-x.d)};
    }
    template <class S>
    Tangent<S> pow(const Tangent<S> &a, const Tangent<S> &b)
    {
        return exp(b * log(a));
    }
    template <class S>
    Tangent<S> pow(const Tangent<S> &a, double b)
    {
        using std::pow;
        using tangent_detail::s;
        const S p = s<S>(pow(a.v, b));
        return {p, s<S>(b * pow(a.v, b - 1.0) * a.d)};
    }
    template <class S>
    Tangent<S> pow(double a, const Tangent<S> &b)
    {
        return exp(b * std::log(a));
    }
    /// max / min take the tangent of the side they pick: the tape's rule.
    template <class S>
    Tangent<S> max(const Tangent<S> &a, const Tangent<S> &b)
    {
        return tangent_detail::val(a) > tangent_detail::val(b) ? a : b;
    }
    template <class S>
    Tangent<S> max(const Tangent<S> &a, double b)
    {
        return tangent_detail::val(a) > b ? a : Tangent<S>(b);
    }
    template <class S>
    Tangent<S> max(double a, const Tangent<S> &b)
    {
        return a > tangent_detail::val(b) ? Tangent<S>(a) : b;
    }
    template <class S>
    Tangent<S> min(const Tangent<S> &a, const Tangent<S> &b)
    {
        return tangent_detail::val(a) < tangent_detail::val(b) ? a : b;
    }
    template <class S>
    Tangent<S> min(const Tangent<S> &a, double b)
    {
        return tangent_detail::val(a) < b ? a : Tangent<S>(b);
    }
    template <class S>
    Tangent<S> min(double a, const Tangent<S> &b)
    {
        return a < tangent_detail::val(b) ? Tangent<S>(a) : b;
    }
    template <class S>
    Tangent<S> normal_cdf(const Tangent<S> &x)
    {
        using tangent_detail::s;
        if constexpr (std::is_same_v<S, double>)
            return {quantModeling::norm_cdf(x.v), quantModeling::norm_pdf(x.v) * x.d};
        else
            return {s<S>(normal_cdf(x.v)), s<S>(normal_dens(x.v) * x.d)};
    }

    // ── comparisons: values only ────────────────────────────────────────────

#define QM_TANGENT_COMPARISON(op)                                \
    template <class S>                                           \
    bool operator op(const Tangent<S> &a, const Tangent<S> &b)   \
    {                                                            \
        return tangent_detail::val(a) op tangent_detail::val(b); \
    }                                                            \
    template <class S>                                           \
    bool operator op(const Tangent<S> &a, double b)              \
    {                                                            \
        return tangent_detail::val(a) op b;                      \
    }                                                            \
    template <class S>                                           \
    bool operator op(double a, const Tangent<S> &b)              \
    {                                                            \
        return a op tangent_detail::val(b);                      \
    }
    QM_TANGENT_COMPARISON(==)
    QM_TANGENT_COMPARISON(!=)
    QM_TANGENT_COMPARISON(<)
    QM_TANGENT_COMPARISON(<=)
    QM_TANGENT_COMPARISON(>)
    QM_TANGENT_COMPARISON(>=)
#undef QM_TANGENT_COMPARISON

} // namespace quantModeling::aad

namespace quantModeling
{
    template <class S>
    double to_double(const aad::Tangent<S> &x)
    {
        return to_double(x.v);
    }
} // namespace quantModeling

#endif // QM_AAD_TANGENT_HPP
