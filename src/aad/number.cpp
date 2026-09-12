#include "quantModeling/aad/number.hpp"

#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <iterator>

namespace quantModeling::aad
{
    namespace
    {
        thread_local Tape default_tape;
    } // namespace

    thread_local Tape *Number::tape = &default_tape;

    Number::Number(Node &arg, double val)
        : value_(val)
    {
        create_node<1>();
        node_->arg_adjoints_[0] = &arg.adjoint();
    }

    Number::Number(Node &lhs, Node &rhs, double val)
        : value_(val)
    {
        create_node<2>();
        node_->arg_adjoints_[0] = &lhs.adjoint();
        node_->arg_adjoints_[1] = &rhs.adjoint();
    }

    Number::Number(double val)
        : value_(val)
    {
        create_node<0>();
    }

    Number &Number::operator=(double val)
    {
        value_ = val;
        create_node<0>();
        return *this;
    }

    void Number::put_on_tape()
    {
        create_node<0>();
    }

    Number &Number::operator+=(const Number &rhs)
    {
        return *this = *this + rhs;
    }
    Number &Number::operator+=(double rhs)
    {
        return *this = *this + rhs;
    }
    Number &Number::operator-=(const Number &rhs)
    {
        return *this = *this - rhs;
    }
    Number &Number::operator-=(double rhs)
    {
        return *this = *this - rhs;
    }
    Number &Number::operator*=(const Number &rhs)
    {
        return *this = *this * rhs;
    }
    Number &Number::operator*=(double rhs)
    {
        return *this = *this * rhs;
    }
    Number &Number::operator/=(const Number &rhs)
    {
        return *this = *this / rhs;
    }
    Number &Number::operator/=(double rhs)
    {
        return *this = *this / rhs;
    }

    void Number::propagate_adjoints(Tape::iterator from, Tape::iterator to)
    {
        Tape::iterator it = from;
        while (it != to)
        {
            it->propagate_one();
            --it;
        }
        it->propagate_one();
    }

    void Number::propagate_to_start()
    {
        adjoint() = 1.0;
        propagate_adjoints(tape->find(node_), tape->begin());
    }

    void Number::propagate_to_mark()
    {
        adjoint() = 1.0;
        propagate_adjoints(tape->find(node_), tape->mark_it());
    }

    void Number::propagate_mark_to_start()
    {
        propagate_adjoints(std::prev(tape->mark_it()), tape->begin());
    }

    // ------------------------------------------------------------- + - * /

    Number operator+(const Number &lhs, const Number &rhs)
    {
        Number result(*lhs.node_, *rhs.node_, lhs.value_ + rhs.value_);
        result.left_der() = 1.0;
        result.right_der() = 1.0;
        return result;
    }
    Number operator+(const Number &lhs, double rhs)
    {
        Number result(*lhs.node_, lhs.value_ + rhs);
        result.derivative() = 1.0;
        return result;
    }
    Number operator+(double lhs, const Number &rhs)
    {
        Number result(*rhs.node_, lhs + rhs.value_);
        result.derivative() = 1.0;
        return result;
    }

    Number operator-(const Number &lhs, const Number &rhs)
    {
        Number result(*lhs.node_, *rhs.node_, lhs.value_ - rhs.value_);
        result.left_der() = 1.0;
        result.right_der() = -1.0;
        return result;
    }
    Number operator-(const Number &lhs, double rhs)
    {
        Number result(*lhs.node_, lhs.value_ - rhs);
        result.derivative() = 1.0;
        return result;
    }
    Number operator-(double lhs, const Number &rhs)
    {
        Number result(*rhs.node_, lhs - rhs.value_);
        result.derivative() = -1.0;
        return result;
    }
    Number operator-(const Number &x)
    {
        Number result(*x.node_, -x.value_);
        result.derivative() = -1.0;
        return result;
    }

    Number operator*(const Number &lhs, const Number &rhs)
    {
        Number result(*lhs.node_, *rhs.node_, lhs.value_ * rhs.value_);
        result.left_der() = rhs.value_;
        result.right_der() = lhs.value_;
        return result;
    }
    Number operator*(const Number &lhs, double rhs)
    {
        Number result(*lhs.node_, lhs.value_ * rhs);
        result.derivative() = rhs;
        return result;
    }
    Number operator*(double lhs, const Number &rhs)
    {
        Number result(*rhs.node_, lhs * rhs.value_);
        result.derivative() = lhs;
        return result;
    }

    Number operator/(const Number &lhs, const Number &rhs)
    {
        const double v = lhs.value_ / rhs.value_;
        Number result(*lhs.node_, *rhs.node_, v);
        result.left_der() = 1.0 / rhs.value_;
        result.right_der() = -v / rhs.value_;
        return result;
    }
    Number operator/(const Number &lhs, double rhs)
    {
        Number result(*lhs.node_, lhs.value_ / rhs);
        result.derivative() = 1.0 / rhs;
        return result;
    }
    Number operator/(double lhs, const Number &rhs)
    {
        const double v = lhs / rhs.value_;
        Number result(*rhs.node_, v);
        result.derivative() = -v / rhs.value_;
        return result;
    }

    // --------------------------------------------------------- pow, max, min

    Number pow(const Number &x, const Number &y)
    {
        const double v = std::pow(x.value_, y.value_);
        Number result(*x.node_, *y.node_, v);
        result.left_der() = y.value_ * v / x.value_;
        result.right_der() = v * std::log(x.value_);
        return result;
    }
    Number pow(const Number &x, double y)
    {
        const double v = std::pow(x.value_, y);
        Number result(*x.node_, v);
        result.derivative() = y * v / x.value_;
        return result;
    }
    Number pow(double x, const Number &y)
    {
        const double v = std::pow(x, y.value_);
        Number result(*y.node_, v);
        result.derivative() = v * std::log(x);
        return result;
    }

    Number max(const Number &x, const Number &y)
    {
        const bool x_wins = x.value_ > y.value_;
        Number result(*x.node_, *y.node_, x_wins ? x.value_ : y.value_);
        result.left_der() = x_wins ? 1.0 : 0.0;
        result.right_der() = x_wins ? 0.0 : 1.0;
        return result;
    }
    Number max(const Number &x, double y)
    {
        const bool x_wins = x.value_ > y;
        Number result(*x.node_, x_wins ? x.value_ : y);
        result.derivative() = x_wins ? 1.0 : 0.0;
        return result;
    }
    Number max(double x, const Number &y)
    {
        const bool y_wins = y.value_ > x;
        Number result(*y.node_, y_wins ? y.value_ : x);
        result.derivative() = y_wins ? 1.0 : 0.0;
        return result;
    }

    Number min(const Number &x, const Number &y)
    {
        const bool x_wins = x.value_ < y.value_;
        Number result(*x.node_, *y.node_, x_wins ? x.value_ : y.value_);
        result.left_der() = x_wins ? 1.0 : 0.0;
        result.right_der() = x_wins ? 0.0 : 1.0;
        return result;
    }
    Number min(const Number &x, double y)
    {
        const bool x_wins = x.value_ < y;
        Number result(*x.node_, x_wins ? x.value_ : y);
        result.derivative() = x_wins ? 1.0 : 0.0;
        return result;
    }
    Number min(double x, const Number &y)
    {
        const bool y_wins = y.value_ < x;
        Number result(*y.node_, y_wins ? y.value_ : x);
        result.derivative() = y_wins ? 1.0 : 0.0;
        return result;
    }

    // ---------------------------------------------------- unary math library

    Number exp(const Number &x)
    {
        const double v = std::exp(x.value_);
        Number result(*x.node_, v);
        result.derivative() = v;
        return result;
    }
    Number log(const Number &x)
    {
        const double v = std::log(x.value_);
        Number result(*x.node_, v);
        result.derivative() = 1.0 / x.value_;
        return result;
    }
    Number sqrt(const Number &x)
    {
        const double v = std::sqrt(x.value_);
        Number result(*x.node_, v);
        result.derivative() = 0.5 / v;
        return result;
    }
    Number fabs(const Number &x)
    {
        Number result(*x.node_, std::fabs(x.value_));
        result.derivative() = x.value_ >= 0.0 ? 1.0 : -1.0;
        return result;
    }
    Number normal_dens(const Number &x)
    {
        const double v = quantModeling::norm_pdf(x.value_);
        Number result(*x.node_, v);
        result.derivative() = -x.value_ * v;
        return result;
    }
    Number normal_cdf(const Number &x)
    {
        const double v = quantModeling::norm_cdf(x.value_);
        Number result(*x.node_, v);
        result.derivative() = quantModeling::norm_pdf(x.value_);
        return result;
    }

    // ------------------------------------------------------------ comparisons

    bool operator==(const Number &lhs, const Number &rhs)
    {
        return lhs.value() == rhs.value();
    }
    bool operator==(const Number &lhs, double rhs)
    {
        return lhs.value() == rhs;
    }
    bool operator==(double lhs, const Number &rhs)
    {
        return lhs == rhs.value();
    }
    bool operator!=(const Number &lhs, const Number &rhs)
    {
        return lhs.value() != rhs.value();
    }
    bool operator!=(const Number &lhs, double rhs)
    {
        return lhs.value() != rhs;
    }
    bool operator!=(double lhs, const Number &rhs)
    {
        return lhs != rhs.value();
    }
    bool operator<(const Number &lhs, const Number &rhs)
    {
        return lhs.value() < rhs.value();
    }
    bool operator<(const Number &lhs, double rhs)
    {
        return lhs.value() < rhs;
    }
    bool operator<(double lhs, const Number &rhs)
    {
        return lhs < rhs.value();
    }
    bool operator<=(const Number &lhs, const Number &rhs)
    {
        return lhs.value() <= rhs.value();
    }
    bool operator<=(const Number &lhs, double rhs)
    {
        return lhs.value() <= rhs;
    }
    bool operator<=(double lhs, const Number &rhs)
    {
        return lhs <= rhs.value();
    }
    bool operator>(const Number &lhs, const Number &rhs)
    {
        return lhs.value() > rhs.value();
    }
    bool operator>(const Number &lhs, double rhs)
    {
        return lhs.value() > rhs;
    }
    bool operator>(double lhs, const Number &rhs)
    {
        return lhs > rhs.value();
    }
    bool operator>=(const Number &lhs, const Number &rhs)
    {
        return lhs.value() >= rhs.value();
    }
    bool operator>=(const Number &lhs, double rhs)
    {
        return lhs.value() >= rhs;
    }
    bool operator>=(double lhs, const Number &rhs)
    {
        return lhs >= rhs.value();
    }

} // namespace quantModeling::aad
