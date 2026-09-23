#include "quantModeling/aad/number.hpp"

#include <iterator>

namespace quantModeling::aad
{
    namespace
    {
        thread_local Tape default_tape;
    } // namespace

    thread_local Tape *Number::tape = &default_tape;

    Number::Number(double val)
        : value_(val)
    {
        node_ = tape->record_node<0>();
    }

    Number &Number::operator=(double val)
    {
        value_ = val;
        node_ = tape->record_node<0>();
        return *this;
    }

    void Number::put_on_tape()
    {
        node_ = tape->record_node<0>();
    }

    // -------------------------------------------------- compound += double

    Number &Number::operator+=(double rhs)
    {
        return *this = *this + rhs;
    }
    Number &Number::operator-=(double rhs)
    {
        return *this = *this - rhs;
    }
    Number &Number::operator*=(double rhs)
    {
        return *this = *this * rhs;
    }
    Number &Number::operator/=(double rhs)
    {
        return *this = *this / rhs;
    }

    // ------------------------------------------------------- propagation

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

    // -------------------------------------------------- multi-adjoint (17f)

    void Number::propagate_adjoints_multi(Tape::iterator from, Tape::iterator to)
    {
        Tape::iterator it = from;
        while (it != to)
        {
            it->propagate_all();
            --it;
        }
        it->propagate_all();
    }

    void Number::propagate_to_start_multi()
    {
        propagate_adjoints_multi(std::prev(tape->end()), tape->begin());
    }

    void Number::propagate_to_mark_multi()
    {
        propagate_adjoints_multi(std::prev(tape->end()), tape->mark_it());
    }

    void Number::propagate_mark_to_start_multi()
    {
        propagate_adjoints_multi(std::prev(tape->mark_it()), tape->begin());
    }

} // namespace quantModeling::aad
