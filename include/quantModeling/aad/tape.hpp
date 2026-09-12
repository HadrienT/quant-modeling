#ifndef QM_AAD_TAPE_HPP
#define QM_AAD_TAPE_HPP

#include "quantModeling/aad/blocklist.hpp"
#include "quantModeling/aad/node.hpp"
#include "quantModeling/core/platform.hpp"

#include <algorithm>
#include <cstddef>

namespace quantModeling::aad
{

    /// Nodes per block; derivatives and argument-pointer slots per block;
    /// multi-adjoint slots per block. The book's values.
    inline constexpr std::size_t BLOCKSIZE = 16384;
    inline constexpr std::size_t ADJSIZE = 32768;
    inline constexpr std::size_t DATASIZE = 65536;

    /**
     * @brief The tape: every operation recorded on Number values, in order.
     *
     * record_node<N>() is the only write path: N is a template parameter, so
     * the arity of an operation is known at compile time and the `if
     * constexpr` below costs nothing at runtime. Four blocklists back it --
     * nodes themselves, their local derivatives, their argument-adjoint
     * pointers, and (once `multi` is turned on, lot 17f) their multi-adjoint
     * rows -- so that every one of those has the stable-address, no-alloc
     * property blocklist provides.
     *
     * `pad_` keeps two tapes belonging to two threads from sharing a cache
     * line (see core/platform.hpp's QM_CACHELINE) once the tape is
     * thread_local (aad/number.hpp) and simulate_parallel_aad (lot 17d) gives
     * one tape per thread.
     */
    class Tape
    {
        static bool multi; // lot 17f: differentiating several results at once

        blocklist<double, ADJSIZE> adjoints_multi_;
        blocklist<double, DATASIZE> derivatives_;
        blocklist<double *, DATASIZE> arg_ptrs_;
        blocklist<Node, BLOCKSIZE> nodes_;
        alignas(QM_CACHELINE) char pad_[QM_CACHELINE]{};

      public:
        Tape() = default;
        Tape(const Tape &) = delete;
        Tape &operator=(const Tape &) = delete;

        template <std::size_t N>
        Node *record_node()
        {
            Node *node = nodes_.emplace_back(N);
            if constexpr (N > 0)
            {
                node->derivatives_ = derivatives_.template emplace_back_multi<N>();
                node->arg_adjoints_ = arg_ptrs_.template emplace_back_multi<N>();
            }
            if (multi)
            {
                node->adjoints_multi_ = adjoints_multi_.emplace_back_multi(Node::num_adj);
                std::fill_n(node->adjoints_multi_, Node::num_adj, 0.0);
            }
            return node;
        }

        using iterator = blocklist<Node, BLOCKSIZE>::iterator;

        iterator begin() { return nodes_.begin(); }
        iterator end() { return nodes_.end(); }
        iterator mark_it() { return nodes_.mark_it(); }
        iterator find(Node *node) { return nodes_.find(node); }

        /// Zero the adjoint of every node currently on the tape.
        void reset_adjoints();
        /// Zero the adjoint of every node *before* the mark only -- the
        /// parameters and model precomputations, whose adjoints accumulate
        /// across paths within one mark epoch and must be cleared between
        /// risk batches (blueprint §7.4); nodes after the mark are reset for
        /// free by rewind_to_mark() + placement-new instead.
        void reset_adjoints_before_mark();

        /// Frees every block of every blocklist.
        void clear();
        /// Back to the very first node. Memory kept.
        void rewind();
        /// Marks the current position (the boundary between "recorded once"
        /// and "recorded per path").
        void mark();
        /// Back to the mark. Memory kept.
        void rewind_to_mark();

        static bool is_multi() { return multi; }
        static void set_multi(bool value) { multi = value; }

        /// Diagnostic only (see blocklist::block_count): total blocks
        /// allocated across the four blocklists backing this tape.
        std::size_t block_count() const
        {
            return adjoints_multi_.block_count() + derivatives_.block_count() +
                   arg_ptrs_.block_count() + nodes_.block_count();
        }

        /// Caps each of the four blocklists at `bytes_per_pool` (not the
        /// tape's total -- each pool has its own per-block size, so a single
        /// shared byte budget per pool is the simplest thing that is still
        /// easy to reason about: "this tape uses at most ~4x this many
        /// bytes"). See blocklist::DEFAULT_BLOCKLIST_MAX_BYTES for the
        /// out-of-the-box ceiling and why it exists. A caller sizing a
        /// thread pool (lot 17d) -- one tape per thread -- should set this
        /// low enough that n_threads * approx_max_bytes() stays under the
        /// machine's real budget, since the default is sized for one path
        /// in isolation, not for dozens of threads sharing the same RAM.
        void set_max_bytes(std::size_t bytes_per_pool)
        {
            adjoints_multi_.set_max_bytes(bytes_per_pool);
            derivatives_.set_max_bytes(bytes_per_pool);
            arg_ptrs_.set_max_bytes(bytes_per_pool);
            nodes_.set_max_bytes(bytes_per_pool);
        }

        /// Upper bound on this tape's own memory (sum of the four pools'
        /// ceilings) -- what set_max_bytes() actually bought you.
        std::size_t approx_max_bytes() const
        {
            return adjoints_multi_.max_bytes() + derivatives_.max_bytes() +
                   arg_ptrs_.max_bytes() + nodes_.max_bytes();
        }
    };

} // namespace quantModeling::aad

#endif // QM_AAD_TAPE_HPP
