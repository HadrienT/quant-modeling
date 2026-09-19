#include "quantModeling/utils/thread_pool.hpp"

#include <chrono>

namespace quantModeling
{

    thread_local std::size_t ThreadPool::tls_num_ = 0;

    ThreadPool::~ThreadPool()
    {
        stop();
    }

    std::size_t ThreadPool::default_thread_count()
    {
        const unsigned hc = std::thread::hardware_concurrency();
        return hc > 1 ? static_cast<std::size_t>(hc - 1) : 1;
    }

    void ThreadPool::start(std::size_t n)
    {
        if (!threads_.empty())
            return;
        queue_.reset();
        threads_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            threads_.emplace_back([this, i]
                                  { worker_loop(i + 1); });
    }

    void ThreadPool::stop()
    {
        if (threads_.empty())
            return;
        queue_.interrupt();
        for (std::thread &t : threads_)
            if (t.joinable())
                t.join();
        threads_.clear();
    }

    void ThreadPool::worker_loop(std::size_t index)
    {
        tls_num_ = index;
        while (true)
        {
            Task task;
            queue_.pop(task);
            if (!task.valid())
                return; // interrupted, nothing left in the queue
            task();
        }
    }

    bool ThreadPool::active_wait(TaskHandle &handle)
    {
        while (handle.wait_for(std::chrono::seconds(0)) !=
               std::future_status::ready)
        {
            Task task;
            if (queue_.try_pop(task))
                task();
            else
                std::this_thread::yield();
        }
        return handle.get();
    }

} // namespace quantModeling
