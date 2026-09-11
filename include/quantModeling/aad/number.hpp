#ifndef QM_AAD_NUMBER_HPP
#define QM_AAD_NUMBER_HPP

#include "quantModeling/aad/node.hpp"
#include "quantModeling/aad/tape.hpp"

#include <cstddef>

namespace quantModeling::aad
{

    /**
     * @brief The differentiable number: a value plus a pointer to the tape
     *        node that recorded how it was computed.
     *
     * 16 bytes, two fields. The conversion to double is explicit on purpose:
     * an implicit one would silently drop a value's dependency on the tape
     * with no warning -- the exact trap described in the blueprint's §0 (a
     * model instantiated with T = Number that compiles and returns zero
     * sensitivities), only quieter.
     *
     * Comparisons (<, >, ==...) compare *values* and record nothing: control
     * flow is not differentiated (blueprint §5.3). A digitale's or a
     * barrier's pathwise delta through a hard comparison is therefore zero
     * almost everywhere; smoothing (fuzzy logic, lot 16) is the answer, not a
     * bug here.
     */
    class Number
    {
        double value_;
        Node *node_;

        template <std::size_t N>
        void create_node()
        {
            node_ = tape->record_node<N>();
        }

        double &derivative() { return node_->derivatives_[0]; }
        double &left_der() { return node_->derivatives_[0]; }
        double &right_der() { return node_->derivatives_[1]; }

        /// Unary node: one argument, its adjoint wired at construction.
        Number(Node &arg, double val);
        /// Binary node: two arguments.
        Number(Node &lhs, Node &rhs, double val);

      public:
        /// The tape of the current thread. thread_local rather than passed
        /// as an argument because an operator overload (`a * b`) has no
        /// argument to carry it in -- see blueprint ADR-A2 for why this is
        /// the one accepted exception to "no mutable static state" in the
        /// core.
        static thread_local Tape *tape;

        Number() = default; // uninitialized: no value, no node
        explicit Number(double val);
        Number &operator=(double val);

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

        Number &operator+=(const Number &rhs);
        Number &operator+=(double rhs);
        Number &operator-=(const Number &rhs);
        Number &operator-=(double rhs);
        Number &operator*=(const Number &rhs);
        Number &operator*=(double rhs);
        Number &operator/=(const Number &rhs);
        Number &operator/=(double rhs);

        static void propagate_adjoints(Tape::iterator from, Tape::iterator to);
        void propagate_to_start();
        void propagate_to_mark();
        static void propagate_mark_to_start();

        friend Number operator+(const Number &, const Number &);
        friend Number operator+(const Number &, double);
        friend Number operator+(double, const Number &);
        friend Number operator-(const Number &, const Number &);
        friend Number operator-(const Number &, double);
        friend Number operator-(double, const Number &);
        friend Number operator-(const Number &);
        friend Number operator*(const Number &, const Number &);
        friend Number operator*(const Number &, double);
        friend Number operator*(double, const Number &);
        friend Number operator/(const Number &, const Number &);
        friend Number operator/(const Number &, double);
        friend Number operator/(double, const Number &);
        friend Number pow(const Number &, const Number &);
        friend Number pow(const Number &, double);
        friend Number pow(double, const Number &);
        friend Number max(const Number &, const Number &);
        friend Number max(const Number &, double);
        friend Number max(double, const Number &);
        friend Number min(const Number &, const Number &);
        friend Number min(const Number &, double);
        friend Number min(double, const Number &);
        friend Number exp(const Number &);
        friend Number log(const Number &);
        friend Number sqrt(const Number &);
        friend Number fabs(const Number &);
        friend Number normal_dens(const Number &);
        friend Number normal_cdf(const Number &);
    };

    Number operator+(const Number &lhs, const Number &rhs);
    Number operator+(const Number &lhs, double rhs);
    Number operator+(double lhs, const Number &rhs);
    Number operator-(const Number &lhs, const Number &rhs);
    Number operator-(const Number &lhs, double rhs);
    Number operator-(double lhs, const Number &rhs);
    Number operator-(const Number &x);
    Number operator*(const Number &lhs, const Number &rhs);
    Number operator*(const Number &lhs, double rhs);
    Number operator*(double lhs, const Number &rhs);
    Number operator/(const Number &lhs, const Number &rhs);
    Number operator/(const Number &lhs, double rhs);
    Number operator/(double lhs, const Number &rhs);

    /// pow, max, min are genuinely binary in the book's derivative table
    /// (§5.2): even the mixed overloads still record a single *unary* node
    /// (the other side is a compile-time constant), never a binary node with
    /// a wasted leaf for the double.
    Number pow(const Number &x, const Number &y);
    Number pow(const Number &x, double y);
    Number pow(double x, const Number &y);
    Number max(const Number &x, const Number &y);
    Number max(const Number &x, double y);
    Number max(double x, const Number &y);
    Number min(const Number &x, const Number &y);
    Number min(const Number &x, double y);
    Number min(double x, const Number &y);

    Number exp(const Number &x);
    Number log(const Number &x);
    Number sqrt(const Number &x);
    Number fabs(const Number &x);
    /// phi(x): the standard normal density.
    Number normal_dens(const Number &x);
    /// Phi(x): the standard normal cdf.
    Number normal_cdf(const Number &x);

    bool operator==(const Number &lhs, const Number &rhs);
    bool operator==(const Number &lhs, double rhs);
    bool operator==(double lhs, const Number &rhs);
    bool operator!=(const Number &lhs, const Number &rhs);
    bool operator!=(const Number &lhs, double rhs);
    bool operator!=(double lhs, const Number &rhs);
    bool operator<(const Number &lhs, const Number &rhs);
    bool operator<(const Number &lhs, double rhs);
    bool operator<(double lhs, const Number &rhs);
    bool operator<=(const Number &lhs, const Number &rhs);
    bool operator<=(const Number &lhs, double rhs);
    bool operator<=(double lhs, const Number &rhs);
    bool operator>(const Number &lhs, const Number &rhs);
    bool operator>(const Number &lhs, double rhs);
    bool operator>(double lhs, const Number &rhs);
    bool operator>=(const Number &lhs, const Number &rhs);
    bool operator>=(const Number &lhs, double rhs);
    bool operator>=(double lhs, const Number &rhs);

} // namespace quantModeling::aad

#endif // QM_AAD_NUMBER_HPP
