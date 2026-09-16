#ifndef QM_UTILS_THREAD_POOL_HPP
#define QM_UTILS_THREAD_POOL_HPP

#include "quantModeling/utils/concurrent_queue.hpp"

#include <cstddef>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace quantModeling
{

    using Task = std::packaged_task<bool()>;
    using TaskHandle = std::future<bool>;

    /**
     * @brief The book's ThreadPool (blueprint/wp/17-aad.md §8.1), owned by
     *        the caller rather than a singleton (ADR-A7): the core forbids
     *        global singletons, and a pool is a convenience, not an
     *        algorithmic necessity -- `simulate_parallel_aad` takes one by
     *        reference instead.
     *
     * `thread_num()` reads a `thread_local`, so it stays a static method:
     * 0 on any thread the pool did not start (the caller/main thread, by
     * the book's own convention), 1..num_threads() on a pool worker.
     *
     * `active_wait()` is the point of the whole design: the calling thread
     * does not block waiting for its tasks to finish -- it drains the same
     * queue itself. No core sits idle, and there is no deadlock risk from a
     * task that itself spawns and waits on further tasks.
     */
    class ThreadPool
    {
      public:
        ThreadPool() = default;
        ~ThreadPool();

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        /// Starts n worker threads (default: hardware_concurrency() - 1, so
        /// the calling thread itself makes up the last one). A no-op if
        /// already started; call stop() first to restart with a different n.
        void start(std::size_t n = default_thread_count());

        /// Interrupts and joins every worker thread. Safe to call when not
        /// started. The pool can be start()ed again afterwards.
        void stop();

        std::size_t num_threads() const { return threads_.size(); }

        static std::size_t thread_num() { return tls_num_; }

        template <class F>
        TaskHandle spawn_task(F &&f)
        {
            Task task(std::forward<F>(f));
            TaskHandle handle = task.get_future();
            queue_.push(std::move(task));
            return handle;
        }

        /// Executes queued tasks on the calling thread until `handle` is
        /// ready, then returns its result. Safe to call with zero worker
        /// threads started -- the calling thread then does all the work.
        bool active_wait(TaskHandle &handle);

      private:
        static std::size_t default_thread_count();
        void worker_loop(std::size_t index);

        ConcurrentQueue<Task> queue_;
        std::vector<std::thread> threads_;
        static thread_local std::size_t tls_num_;
    };

} // namespace quantModeling

#endif // QM_UTILS_THREAD_POOL_HPP
