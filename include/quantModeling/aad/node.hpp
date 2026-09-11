#ifndef QM_AAD_NODE_HPP
#define QM_AAD_NODE_HPP

#include <cstddef>

namespace quantModeling::aad
{

    class Tape;
    class Number;

    /**
     * @brief One node of the tape: one elementary operation y = f(args...).
     *
     * Two choices from the book kept exactly as written:
     *  - a node stores pointers to its *arguments'* adjoints, not pointers to
     *    the argument nodes themselves, so propagation is a loop of
     *    multiply-adds with no extra indirection;
     *  - `propagate_one()` returns immediately when the adjoint is still
     *    zero. That short-circuits whole dead branches of the computation
     *    (unreached script branches, disabled paths) -- a real saving, not a
     *    micro-optimisation.
     *
     * 40 bytes per node, plus 16 bytes per argument (one derivative, one
     * pointer), carved out of the tape's blocklists rather than owned here.
     */
    class Node
    {
        friend class Tape;
        friend class Number;

        const std::size_t n_; // number of arguments
        double adjoint_ = 0.0; // single adjoint
        double *derivatives_ = nullptr; // n local derivatives d f / d arg_i
        double **arg_adjoints_ = nullptr; // n pointers to the arguments' adjoints
        double *adjoints_multi_ = nullptr; // multi-adjoint row, see propagate_all()

      public:
        /// Width of the multi-adjoint row every node carries once
        /// Tape::multi is turned on (differentiating several results in one
        /// backward pass -- lot 17f). 1 until then.
        static std::size_t num_adj;

        explicit Node(std::size_t n = 0) : n_(n) {}

        double &adjoint() { return adjoint_; }
        double adjoint() const { return adjoint_; }

        std::size_t n_args() const { return n_; }

        void propagate_one()
        {
            if (!n_ || adjoint_ == 0.0)
                return; // leaf, or a zero adjoint: nothing downstream needs it
            for (std::size_t i = 0; i < n_; ++i)
                *arg_adjoints_[i] += derivatives_[i] * adjoint_;
        }

        /// Same loop as propagate_one(), over every one of the num_adj
        /// adjoints carried in adjoints_multi_ -- lot 17f wires arg_adjoints_
        /// to point at the arguments' multi-adjoint rows instead of their
        /// scalar adjoint_ when Tape::multi is set, which is what makes this
        /// correct; not yet exercised before that lot.
        void propagate_all()
        {
            if (!n_)
                return;
            for (std::size_t i = 0; i < n_; ++i)
            {
                double *row = arg_adjoints_[i];
                for (std::size_t j = 0; j < num_adj; ++j)
                    row[j] += derivatives_[i] * adjoints_multi_[j];
            }
        }
    };

} // namespace quantModeling::aad

#endif // QM_AAD_NODE_HPP
