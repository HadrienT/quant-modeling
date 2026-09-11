#ifndef QM_AAD_BLOCKLIST_HPP
#define QM_AAD_BLOCKLIST_HPP

#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace quantModeling::aad
{

    /**
     * @brief Block-based memory for the tape (Savine, chapter on AAD memory).
     *
     * Not a std::vector, for the two requirements a tape has:
     *  - stable addresses: nodes point at each other's adjoints by raw
     *    pointer, and a std::vector that reallocates would invalidate every
     *    one of them;
     *  - no allocation in the hot loop: rewinding a path does not free
     *    anything, blocks stay in place and are reused by the next one.
     *
     * Each block is raw, uninitialized storage: elements are placement-new'd
     * in (emplace_back / emplace_back_multi) and never explicitly destroyed
     * on rewind -- rewinding only moves the "next free slot" pointer back,
     * and the next placement-new overwrites what was there. That is safe
     * here because every T this is instantiated with (double, double*,
     * aad::Node) is trivially destructible, so starting a new object's
     * lifetime over an old one without running its (no-op) destructor first
     * is well defined.
     */
    template <class T, std::size_t block_size>
    class blocklist
    {
        using storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
        using block = std::array<storage, block_size>;
        using block_list = std::list<block>;
        using block_iterator = typename block_list::iterator;

        static_assert(std::is_trivially_destructible_v<T>,
                     "blocklist reuses storage without destroying old "
                     "objects: T must be trivially destructible");

        block_list data_;
        block_iterator cur_block_;
        block_iterator last_block_;
        std::size_t next_index_ = 0; // next free slot in cur_block_
        std::size_t last_index_ = 0; // == block_size once cur_block_ is full

        block_iterator marked_block_{};
        std::size_t marked_index_ = 0;

        T *slot(block_iterator b, std::size_t i)
        {
            return std::launder(reinterpret_cast<T *>(&(*b)[i]));
        }

        void new_block()
        {
            data_.emplace_back();
            cur_block_ = last_block_ = std::prev(data_.end());
            next_index_ = 0;
            last_index_ = block_size;
        }

        void next_block()
        {
            if (cur_block_ == last_block_)
                new_block();
            else
            {
                ++cur_block_;
                next_index_ = 0;
                last_index_ = block_size;
            }
        }

      public:
        blocklist() { new_block(); }
        blocklist(const blocklist &) = delete;
        blocklist &operator=(const blocklist &) = delete;

        /// Constructs one T in place with `args`, growing the tape if the
        /// current block is exactly full. Never skips a partial block -- one
        /// element at a time always fits what next_block() just cleared.
        template <class... Args>
        T *emplace_back(Args &&...args)
        {
            if (next_index_ == last_index_)
                next_block();
            T *p = slot(cur_block_, next_index_);
            ::new (static_cast<void *>(p)) T(std::forward<Args>(args)...);
            ++next_index_;
            return p;
        }

        /// n contiguous, default-constructed slots -- the local derivatives
        /// and argument-adjoint pointers of one n-ary node. If fewer than n
        /// slots remain in the current block, that block's tail is left
        /// unused and a fresh block is started: n <= block_size is the
        /// invariant that makes this always succeed in the new block.
        template <std::size_t n>
        T *emplace_back_multi()
        {
            static_assert(n <= block_size, "n must fit in one block");
            return emplace_back_multi(n);
        }

        T *emplace_back_multi(std::size_t n)
        {
            if (n > block_size)
                throw std::invalid_argument("blocklist: n exceeds block_size");
            if (last_index_ - next_index_ < n)
                next_block();
            T *p = slot(cur_block_, next_index_);
            for (std::size_t i = 0; i < n; ++i)
                ::new (static_cast<void *>(p + i)) T();
            next_index_ += n;
            return p;
        }

        /// Frees every block. The only operation that actually releases
        /// memory -- everything else below reuses it.
        void clear()
        {
            data_.clear();
            new_block();
        }

        /// Back to the very first block, first slot. Memory is kept.
        void rewind()
        {
            cur_block_ = data_.begin();
            next_index_ = 0;
            last_index_ = block_size;
        }

        /// Remembers the current position for rewind_to_mark().
        void set_mark()
        {
            marked_block_ = cur_block_;
            marked_index_ = next_index_;
        }

        /// Back to the marked position. Memory is kept.
        void rewind_to_mark()
        {
            cur_block_ = marked_block_;
            next_index_ = marked_index_;
            last_index_ = block_size;
        }

        void memset(unsigned char v = 0)
        {
            for (block &b : data_)
                std::memset(b.data(), v, block_size * sizeof(storage));
        }

        /// Number of blocks currently allocated -- a diagnostic, not part of
        /// the book's API, added so the "no allocation after the first path"
        /// property (blueprint §2) is something a test can actually observe.
        std::size_t block_count() const { return data_.size(); }

        // ------------------------------------------------------------ iteration
        //
        // Bidirectional, in insertion order. Only meaningful when every
        // element was placed with emplace_back() (one at a time, as Tape
        // does for its Node blocklist): every block strictly before
        // cur_block_ is then guaranteed to be full, so incrementing across a
        // block boundary is simply "start the next block at index 0" and
        // decrementing across one is "end of the previous block".
        class iterator
        {
            friend class blocklist;
            block_iterator block_;
            std::size_t index_ = 0;
            blocklist *owner_ = nullptr;

            iterator(blocklist *owner, block_iterator b, std::size_t i)
                : block_(b), index_(i), owner_(owner)
            {
            }

          public:
            using iterator_category = std::bidirectional_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T *;
            using reference = T &;

            iterator() = default;

            T &operator*() const { return *owner_->slot(block_, index_); }
            T *operator->() const { return owner_->slot(block_, index_); }

            iterator &operator++()
            {
                ++index_;
                if (index_ == block_size && block_ != owner_->last_block_)
                {
                    ++block_;
                    index_ = 0;
                }
                return *this;
            }

            iterator &operator--()
            {
                if (index_ == 0)
                {
                    --block_;
                    index_ = block_size - 1;
                }
                else
                {
                    --index_;
                }
                return *this;
            }

            bool operator==(const iterator &other) const
            {
                return block_ == other.block_ && index_ == other.index_;
            }
            bool operator!=(const iterator &other) const { return !(*this == other); }
        };

        iterator begin() { return iterator(this, data_.begin(), 0); }
        iterator end() { return iterator(this, cur_block_, next_index_); }
        iterator mark_it() { return iterator(this, marked_block_, marked_index_); }

        /// Locates the element at address `p` -- used to turn a raw T* (a
        /// Number's node_) back into an iterator so propagation can walk
        /// backward from it. A linear scan over *blocks*, not elements: with
        /// block_size in the tens of thousands this is a handful of pointer
        /// comparisons per path, not a hot loop.
        iterator find(T *p)
        {
            for (block_iterator b = data_.begin(); b != data_.end(); ++b)
            {
                auto *first = reinterpret_cast<const std::byte *>(b->data());
                auto *last = first + block_size * sizeof(storage);
                auto *addr = reinterpret_cast<const std::byte *>(p);
                if (addr >= first && addr < last)
                {
                    const std::size_t index = static_cast<std::size_t>(
                        (addr - first) / sizeof(storage));
                    return iterator(this, b, index);
                }
            }
            throw std::logic_error("blocklist::find: pointer not on this tape");
        }
    };

} // namespace quantModeling::aad

#endif // QM_AAD_BLOCKLIST_HPP
