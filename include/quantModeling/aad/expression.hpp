#ifndef QM_AAD_EXPRESSION_HPP
#define QM_AAD_EXPRESSION_HPP

#include "quantModeling/aad/node.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <cstddef>

namespace quantModeling::aad
{

    /**
     * @brief CRTP base of every expression template node (blueprint
     *        /wp/17-aad.md §10): a lazily-built arithmetic expression over
     *        `Number` leaves, not yet recorded on the tape.
     *
     * `Number` itself derives from `Expression<Number>` (a one-leaf
     * expression), so every operator below is written once, generically,
     * for any combination of `Number` and compound expressions -- there is
     * no separate "Number op Number" vs "Number op BinaryExpression"
     * overload set.
     *
     * **Deviation from the book (ADR-A9), found by a failing test, not by
     * inspection.** The book's `BinaryExpression`/`UnaryExpression` capture
     * their operands *by reference*, and its "never write `auto` for a `T`
     * expression" convention exists because of that: a temporary operand's
     * reference would dangle once used on a later statement. This project's
     * mixed operations (`x * 2.0`) wrap the literal in a `ScalarExpr`
     * constructed *inside* the operator function itself (`operator*(const
     * Expression<L>&, double)` below) -- with reference capture, that
     * `ScalarExpr` is already destroyed by the time the caller's statement
     * even receives the returned expression, dangling reference guaranteed,
     * not just on a later statement. `AADNumber.MixedMultiplicationRecords
     * OneNodeNotTwo` caught this directly (`x.adjoint()` silently came back
     * 0 instead of 2). The fix kept here is to capture every operand *by
     * value* instead: `Number` is 16 bytes and `ScalarExpr` 8, both
     * trivially copyable with no tape interaction in a copy, so the cost is
     * a few stack copies, not a real one -- and it makes the whole class of
     * bug structurally impossible rather than a documented rule to remember
     * (a plain `auto y = a * b;` is safe here, though the codebase's
     * convention of always writing `Number y = a * b;` is kept anyway, for
     * readability and in case a future change reintroduces something
     * non-trivially-copyable).
     */
    template <class E>
    struct Expression
    {
        double value() const { return static_cast<const E &>(*this).value(); }
    };

    /// A raw double, wrapped so it can stand on either side of a
    /// BinaryExpression without ever becoming a tape leaf: num_numbers = 0
    /// means push_adjoint on this side is never invoked (§5.2 -- "mixed
    /// operations record a single *unary* node, never a binary node with a
    /// wasted leaf for the constant"; `x * 2.0` therefore ends up with
    /// `BinaryExpression<Number, OPMult, ScalarExpr>::num_numbers == 1`).
    class ScalarExpr : public Expression<ScalarExpr>
    {
        const double value_;

      public:
        static constexpr std::size_t num_numbers = 0;

        explicit ScalarExpr(double v)
            : value_(v) {}

        double value() const { return value_; }

        template <std::size_t N, std::size_t n>
        void push_adjoint(Node &, double) const
        {
            // Never actually called: every caller guards with
            // `if constexpr (side::num_numbers > 0)` before recursing here.
        }
    };

    /// LHS OP RHS, where either side is a `Number`, a `ScalarExpr`, or
    /// another `BinaryExpression`/`UnaryExpression`. `value_` is computed
    /// eagerly (cheap, and needed by push_adjoint's local-derivative
    /// formulas either way); the tape node itself is recorded only once,
    /// when this expression is finally assigned to a `Number`.
    template <class LHS, class OP, class RHS>
    class BinaryExpression : public Expression<BinaryExpression<LHS, OP, RHS>>
    {
        const double value_;
        const LHS lhs_;
        const RHS rhs_;

      public:
        static constexpr std::size_t num_numbers = LHS::num_numbers + RHS::num_numbers;

        BinaryExpression(const LHS &lhs, const RHS &rhs)
            : value_(OP::eval(lhs.value(), rhs.value())), lhs_(lhs), rhs_(rhs) {}

        double value() const { return value_; }

        /// Recurses into whichever side(s) actually carry a Number leaf,
        /// each contributing its local derivative (chain rule) times the
        /// adjoint flowing in from the caller -- `N` is the arity of the
        /// tape node finally being built (constant across the whole
        /// recursion), `n` is this call's starting slot within it.
        template <std::size_t N, std::size_t n>
        void push_adjoint(Node &node, double adj) const
        {
            if constexpr (LHS::num_numbers > 0)
                lhs_.template push_adjoint<N, n>(
                    node, adj * OP::left_derivative(lhs_.value(), rhs_.value(), value_));
            if constexpr (RHS::num_numbers > 0)
                rhs_.template push_adjoint<N, n + LHS::num_numbers>(
                    node, adj * OP::right_derivative(lhs_.value(), rhs_.value(), value_));
        }
    };

    /// OP(ARG) -- exp, log, sqrt, fabs, normal_dens, normal_cdf, unary
    /// minus. Same shape as BinaryExpression with one side.
    template <class ARG, class OP>
    class UnaryExpression : public Expression<UnaryExpression<ARG, OP>>
    {
        const double value_;
        const ARG arg_;

      public:
        static constexpr std::size_t num_numbers = ARG::num_numbers;

        explicit UnaryExpression(const ARG &arg)
            : value_(OP::eval(arg.value())), arg_(arg) {}

        double value() const { return value_; }

        template <std::size_t N, std::size_t n>
        void push_adjoint(Node &node, double adj) const
        {
            arg_.template push_adjoint<N, n>(node, adj * OP::derivative(arg_.value(), value_));
        }
    };

    // ── operation tags: value + local derivative(s), blueprint §5.2's table.
    // Reused verbatim by both the binary/unary ops below AND (via
    // number.cpp) nowhere else -- these free functions ARE the only place
    // the derivative table lives now. `x / y` and `sqrt` reuse the already-
    // computed value `v`, same as the book. ──────────────────────────────

    struct OPAdd
    {
        static double eval(double l, double r) { return l + r; }
        static double left_derivative(double, double, double) { return 1.0; }
        static double right_derivative(double, double, double) { return 1.0; }
    };
    struct OPSub
    {
        static double eval(double l, double r) { return l - r; }
        static double left_derivative(double, double, double) { return 1.0; }
        static double right_derivative(double, double, double) { return -1.0; }
    };
    struct OPMult
    {
        static double eval(double l, double r) { return l * r; }
        static double left_derivative(double, double r, double) { return r; }
        static double right_derivative(double l, double, double) { return l; }
    };
    struct OPDiv
    {
        static double eval(double l, double r) { return l / r; }
        static double left_derivative(double, double r, double) { return 1.0 / r; }
        static double right_derivative(double, double r, double v) { return -v / r; }
    };
    struct OPPow
    {
        static double eval(double l, double r) { return std::pow(l, r); }
        static double left_derivative(double l, double r, double v) { return r * v / l; }
        static double right_derivative(double l, double, double v) { return v * std::log(l); }
    };
    struct OPMax
    {
        static double eval(double l, double r) { return l > r ? l : r; }
        static double left_derivative(double l, double r, double) { return l > r ? 1.0 : 0.0; }
        static double right_derivative(double l, double r, double) { return l > r ? 0.0 : 1.0; }
    };
    struct OPMin
    {
        static double eval(double l, double r) { return l < r ? l : r; }
        static double left_derivative(double l, double r, double) { return l < r ? 1.0 : 0.0; }
        static double right_derivative(double l, double r, double) { return l < r ? 0.0 : 1.0; }
    };

    struct OPNeg
    {
        static double eval(double x) { return -x; }
        static double derivative(double, double) { return -1.0; }
    };
    struct OPExp
    {
        static double eval(double x) { return std::exp(x); }
        static double derivative(double, double v) { return v; }
    };
    struct OPLog
    {
        static double eval(double x) { return std::log(x); }
        static double derivative(double x, double) { return 1.0 / x; }
    };
    struct OPSqrt
    {
        static double eval(double x) { return std::sqrt(x); }
        static double derivative(double, double v) { return 0.5 / v; }
    };
    struct OPFabs
    {
        static double eval(double x) { return std::fabs(x); }
        static double derivative(double x, double) { return x >= 0.0 ? 1.0 : -1.0; }
    };
    struct OPNormalDens
    {
        static double eval(double x) { return quantModeling::norm_pdf(x); }
        static double derivative(double x, double v) { return -x * v; }
    };
    struct OPNormalCdf
    {
        static double eval(double x) { return quantModeling::norm_cdf(x); }
        static double derivative(double x, double) { return quantModeling::norm_pdf(x); }
    };

    // ── operators: Expression op Expression, Expression op double, double
    // op Expression -- one triple per operation, mechanically. A `double`
    // side is wrapped in a ScalarExpr, which is what makes the mixed
    // overloads collapse to the unary-node case automatically (num_numbers
    // arithmetic), rather than needing a second, hand-written derivative
    // table the way the pre-ET Number did. ─────────────────────────────────

    template <class L, class R>
    BinaryExpression<L, OPAdd, R> operator+(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPAdd, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPAdd, ScalarExpr> operator+(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPAdd, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPAdd, R> operator+(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPAdd, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    template <class L, class R>
    BinaryExpression<L, OPSub, R> operator-(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPSub, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPSub, ScalarExpr> operator-(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPSub, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPSub, R> operator-(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPSub, R>(ScalarExpr(l), static_cast<const R &>(r));
    }
    template <class E>
    UnaryExpression<E, OPNeg> operator-(const Expression<E> &x)
    {
        return UnaryExpression<E, OPNeg>(static_cast<const E &>(x));
    }

    template <class L, class R>
    BinaryExpression<L, OPMult, R> operator*(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPMult, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPMult, ScalarExpr> operator*(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPMult, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPMult, R> operator*(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPMult, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    template <class L, class R>
    BinaryExpression<L, OPDiv, R> operator/(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPDiv, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPDiv, ScalarExpr> operator/(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPDiv, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPDiv, R> operator/(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPDiv, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    /// pow, max, min are genuinely binary in the derivative table (§5.2):
    /// even the mixed overloads still record a single *unary* node (the
    /// other side is a compile-time constant), never a binary node with a
    /// wasted leaf for the double -- same num_numbers mechanism as +, -, *, /.
    template <class L, class R>
    BinaryExpression<L, OPPow, R> pow(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPPow, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPPow, ScalarExpr> pow(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPPow, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPPow, R> pow(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPPow, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    template <class L, class R>
    BinaryExpression<L, OPMax, R> max(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPMax, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPMax, ScalarExpr> max(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPMax, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPMax, R> max(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPMax, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    template <class L, class R>
    BinaryExpression<L, OPMin, R> min(const Expression<L> &l, const Expression<R> &r)
    {
        return BinaryExpression<L, OPMin, R>(static_cast<const L &>(l), static_cast<const R &>(r));
    }
    template <class L>
    BinaryExpression<L, OPMin, ScalarExpr> min(const Expression<L> &l, double r)
    {
        return BinaryExpression<L, OPMin, ScalarExpr>(static_cast<const L &>(l), ScalarExpr(r));
    }
    template <class R>
    BinaryExpression<ScalarExpr, OPMin, R> min(double l, const Expression<R> &r)
    {
        return BinaryExpression<ScalarExpr, OPMin, R>(ScalarExpr(l), static_cast<const R &>(r));
    }

    template <class E>
    UnaryExpression<E, OPExp> exp(const Expression<E> &x)
    {
        return UnaryExpression<E, OPExp>(static_cast<const E &>(x));
    }
    template <class E>
    UnaryExpression<E, OPLog> log(const Expression<E> &x)
    {
        return UnaryExpression<E, OPLog>(static_cast<const E &>(x));
    }
    template <class E>
    UnaryExpression<E, OPSqrt> sqrt(const Expression<E> &x)
    {
        return UnaryExpression<E, OPSqrt>(static_cast<const E &>(x));
    }
    template <class E>
    UnaryExpression<E, OPFabs> fabs(const Expression<E> &x)
    {
        return UnaryExpression<E, OPFabs>(static_cast<const E &>(x));
    }
    /// phi(x): the standard normal density.
    template <class E>
    UnaryExpression<E, OPNormalDens> normal_dens(const Expression<E> &x)
    {
        return UnaryExpression<E, OPNormalDens>(static_cast<const E &>(x));
    }
    /// Phi(x): the standard normal cdf.
    template <class E>
    UnaryExpression<E, OPNormalCdf> normal_cdf(const Expression<E> &x)
    {
        return UnaryExpression<E, OPNormalCdf>(static_cast<const E &>(x));
    }

    // ── comparisons: values only, no node recorded either side (blueprint
    // §5.3 -- control flow is not differentiated). ─────────────────────────

    template <class L, class R>
    bool operator==(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() == r.value();
    }
    template <class L>
    bool operator==(const Expression<L> &l, double r)
    {
        return l.value() == r;
    }
    template <class R>
    bool operator==(double l, const Expression<R> &r)
    {
        return l == r.value();
    }

    template <class L, class R>
    bool operator!=(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() != r.value();
    }
    template <class L>
    bool operator!=(const Expression<L> &l, double r)
    {
        return l.value() != r;
    }
    template <class R>
    bool operator!=(double l, const Expression<R> &r)
    {
        return l != r.value();
    }

    template <class L, class R>
    bool operator<(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() < r.value();
    }
    template <class L>
    bool operator<(const Expression<L> &l, double r)
    {
        return l.value() < r;
    }
    template <class R>
    bool operator<(double l, const Expression<R> &r)
    {
        return l < r.value();
    }

    template <class L, class R>
    bool operator<=(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() <= r.value();
    }
    template <class L>
    bool operator<=(const Expression<L> &l, double r)
    {
        return l.value() <= r;
    }
    template <class R>
    bool operator<=(double l, const Expression<R> &r)
    {
        return l <= r.value();
    }

    template <class L, class R>
    bool operator>(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() > r.value();
    }
    template <class L>
    bool operator>(const Expression<L> &l, double r)
    {
        return l.value() > r;
    }
    template <class R>
    bool operator>(double l, const Expression<R> &r)
    {
        return l > r.value();
    }

    template <class L, class R>
    bool operator>=(const Expression<L> &l, const Expression<R> &r)
    {
        return l.value() >= r.value();
    }
    template <class L>
    bool operator>=(const Expression<L> &l, double r)
    {
        return l.value() >= r;
    }
    template <class R>
    bool operator>=(double l, const Expression<R> &r)
    {
        return l >= r.value();
    }

} // namespace quantModeling::aad

#endif // QM_AAD_EXPRESSION_HPP
