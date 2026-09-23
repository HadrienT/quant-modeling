#ifndef QM_AAD_NUMBER_HPP
#define QM_AAD_NUMBER_HPP

#include "quantModeling/aad/expression.hpp"
#include "quantModeling/aad/node.hpp"
#include "quantModeling/aad/tape.hpp"

#include <cstddef>

namespace quantModeling::aad
{

    /**
     * @brief The differentiable number: a value plus a pointer to the tape
     *        node that recorded how it was computed.
     *
     * 16 bytes, two fields (`Expression<Number>` is an empty CRTP base,
     * contributing nothing under the empty-base optimization). The
     * conversion to double is explicit on purpose: an implicit one would
     * silently drop a value's dependency on the tape with no warning -- the
     * exact trap described in the blueprint's §0 (a model instantiated with
     * T = Number that compiles and returns zero sensitivities), only
     * quieter.
     *
     * Every arithmetic operator (`+`, `*`, `exp`, ...) is a *generic*
     * expression-template free function in expression.hpp, not a member or
     * friend of this class: `Number` participates simply by being a
     * one-leaf `Expression<Number>` (`num_numbers == 1`). What lives here
     * is only what a leaf needs: how to materialize an expression into an
     * actual tape node (the constructor/assignment operators below), and
     * how to read out a value or an adjoint once one exists.
     *
     * Comparisons (<, >, ==...) compare *values* (expression.hpp) and
     * record nothing: control flow is not differentiated (blueprint §5.3).
     * A digitale's or a barrier's pathwise delta through a hard comparison
     * is therefore zero almost everywhere; smoothing (fuzzy logic, lot 16)
     * is the answer, not a bug here.
     */
    class Number : public Expression<Number>
    {
        double value_;
        Node *node_;

      public:
        /// The tape of the current thread. thread_local rather than passed
        /// as an argument because an operator overload (`a * b`) has no
        /// argument to carry it in -- see blueprint ADR-A2 for why this is
        /// the one accepted exception to "no mutable static state" in the
        /// core.
        static thread_local Tape *tape;

        /// A one-leaf expression (blueprint §10): `num_numbers == 1`, and
        /// push_adjoint writes this leaf's own contribution into whatever
        /// node is being built by an enclosing BinaryExpression/
        /// UnaryExpression -- or, when *this* is the outermost thing being
        /// materialized (a leaf assigned straight to a Number), directly
        /// into that brand-new node.
        static constexpr std::size_t num_numbers = 1;

        Number() = default; // uninitialized: no value, no node
        explicit Number(double val);
        Number &operator=(double val);

        /// Materializes any expression into a real tape node: N =
        /// E::num_numbers arguments, one per distinct Number leaf the
        /// expression touches, each with its own precomputed (chain-rule)
        /// local derivative -- blueprint §10's "y = x1*x2 + exp(x3)
        /// records one node, not three".
        template <class E>
        Number(const Expression<E> &e) // NOLINT(*-explicit-constructor) -- must convert implicitly, see expression.hpp
            : value_(e.value())
        {
            node_ = tape->record_node<E::num_numbers>();
            static_cast<const E &>(e).template push_adjoint<E::num_numbers, 0>(*node_, 1.0);
        }

        template <class E>
        Number &operator=(const Expression<E> &e)
        {
            value_ = e.value();
            node_ = tape->record_node<E::num_numbers>();
            static_cast<const E &>(e).template push_adjoint<E::num_numbers, 0>(*node_, 1.0);
            return *this;
        }

        /// This leaf's own contribution to whichever node is being built
        /// around it: slot n's local derivative is whatever adjoint flowed
        /// in from the caller (a leaf contributes itself, unchanged), and
        /// arg_adjoints_[n] points at where that slot's adjoint should
        /// accumulate -- this Number's own node's scalar adjoint normally,
        /// or its multi-adjoint row once Tape::multi is on (lot 17f).
        template <std::size_t N, std::size_t n>
        void push_adjoint(Node &node, double adj) const
        {
            node.derivatives_[n] = adj;
            node.arg_adjoints_[n] = Tape::is_multi() ? node_->adjoints_multi_ : &node_->adjoint();
        }

        /// (Re-)registers this Number as a leaf, keeping its current value.
        /// Used to put a model's parameters on the tape at the start of a
        /// simulation (lot 17b).
        void put_on_tape();

        double value() const { return value_; }
        double &value() { return value_; }
        double adjoint() const { return node_->adjoint(); }
        double &adjoint() { return node_->adjoint(); }
        double &adjoint(std::size_t n) { return node_->adjoints_multi_[n]; }

        explicit operator double() const { return value_; }

        Number &operator+=(double rhs);
        Number &operator-=(double rhs);
        Number &operator*=(double rhs);
        Number &operator/=(double rhs);
        template <class E>
        Number &operator+=(const Expression<E> &rhs) { return *this = *this + rhs; }
        template <class E>
        Number &operator-=(const Expression<E> &rhs) { return *this = *this - rhs; }
        template <class E>
        Number &operator*=(const Expression<E> &rhs) { return *this = *this * rhs; }
        template <class E>
        Number &operator/=(const Expression<E> &rhs) { return *this = *this / rhs; }

        static void propagate_adjoints(Tape::iterator from, Tape::iterator to);
        void propagate_to_start();
        void propagate_to_mark();
        static void propagate_mark_to_start();

        /// Multi-adjoint counterparts (lot 17f): same three entry points,
        /// but walking the tape with Node::propagate_all() -- the whole
        /// num_adj-wide row per node in one pass -- instead of
        /// Node::propagate_one(). There is no single Number to start "from"
        /// when several independent results were seeded at once
        /// (payoffs[i].adjoint(i) = 1 for each i), so these start from the
        /// tape's most recently recorded node rather than from `this`.
        static void propagate_adjoints_multi(Tape::iterator from, Tape::iterator to);
        static void propagate_to_start_multi();
        static void propagate_to_mark_multi();
        static void propagate_mark_to_start_multi();
    };

} // namespace quantModeling::aad

namespace quantModeling
{
    /// Reads out a T's current value as a plain double regardless of which T
    /// this is -- needed wherever templated code must feed a *state*
    /// (typically the current spot) into something that only ever works in
    /// double, such as a grid lookup (models/equity/local_vol_sim_model.hpp):
    /// which cell gets queried is not itself differentiated, only the T-typed
    /// values found there once the cell is chosen.
    inline double to_double(double x)
    {
        return x;
    }
    inline double to_double(const aad::Number &x)
    {
        return x.value();
    }
} // namespace quantModeling

#endif // QM_AAD_NUMBER_HPP
