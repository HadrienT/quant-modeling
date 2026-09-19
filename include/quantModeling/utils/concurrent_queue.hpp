#ifndef QM_UTILS_CONCURRENT_QUEUE_HPP
#define QM_UTILS_CONCURRENT_QUEUE_HPP

#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace quantModeling
{

    /**
     * @brief A thread-safe FIFO queue: mutex + condition_variable + std::queue
     *        (blueprint/wp/17-aad.md §8.1), the building block ThreadPool's
     *        task queue is made of.
     *
     * `pop()` blocks until either an item is available or the queue has been
     * interrupted. On interruption with nothing left to hand out, it leaves
     * `item` untouched (default-constructed) rather than signalling via an
     * exception or a return value -- ThreadPool's worker loop tells the two
     * cases apart by checking `item`'s own validity (a default-constructed
     * `std::packaged_task` is invalid), the same convention the book uses.
     */
    template <class T>
    class ConcurrentQueue
    {
      public:
        void push(T item)
        {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                queue_.push(std::move(item));
            }
            cv_.notify_one();
        }

        /// Non-blocking: pops one item if available, returns false otherwise.
        bool try_pop(T &item)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty())
                return false;
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        /// Blocks until an item is available or the queue is interrupted.
        void pop(T &item)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]
                     { return interrupted_ || !queue_.empty(); });
            if (queue_.empty())
                return; // interrupted with nothing left: item stays untouched
            item = std::move(queue_.front());
            queue_.pop();
        }

        /// Wakes every thread blocked in pop(); does not clear pending items
        /// (a thread already holding the lock still drains them via try_pop
        /// or a subsequent pop() before it sees interrupted_).
        void interrupt()
        {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                interrupted_ = true;
            }
            cv_.notify_all();
        }

        /// Clears interrupted_ and any leftover items, so the same queue can
        /// back a ThreadPool that is stopped and started again.
        void reset()
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            interrupted_ = false;
            std::queue<T> empty;
            queue_.swap(empty);
        }

      private:
        std::queue<T> queue_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool interrupted_ = false;
    };

} // namespace quantModeling

#endif // QM_UTILS_CONCURRENT_QUEUE_HPP
