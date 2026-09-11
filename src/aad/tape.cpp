#include "quantModeling/aad/tape.hpp"

namespace quantModeling::aad
{
    std::size_t Node::num_adj = 1;
    bool Tape::multi = false;

    void Tape::reset_adjoints()
    {
        for (iterator it = begin(); it != end(); ++it)
            it->adjoint() = 0.0;
    }

    void Tape::reset_adjoints_before_mark()
    {
        const iterator stop = mark_it();
        for (iterator it = begin(); it != stop; ++it)
            it->adjoint() = 0.0;
    }

    void Tape::clear()
    {
        adjoints_multi_.clear();
        derivatives_.clear();
        arg_ptrs_.clear();
        nodes_.clear();
    }

    void Tape::rewind()
    {
        adjoints_multi_.rewind();
        derivatives_.rewind();
        arg_ptrs_.rewind();
        nodes_.rewind();
    }

    void Tape::mark()
    {
        adjoints_multi_.set_mark();
        derivatives_.set_mark();
        arg_ptrs_.set_mark();
        nodes_.set_mark();
    }

    void Tape::rewind_to_mark()
    {
        adjoints_multi_.rewind_to_mark();
        derivatives_.rewind_to_mark();
        arg_ptrs_.rewind_to_mark();
        nodes_.rewind_to_mark();
    }

} // namespace quantModeling::aad
