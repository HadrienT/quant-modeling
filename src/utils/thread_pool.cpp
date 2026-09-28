#include "quantModeling/utils/thread_pool.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

namespace quantModeling
{

    std::size_t available_cpus()
    {
        std::size_t n = std::thread::hardware_concurrency();
#if defined(__linux__)
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof(set), &set) == 0)
            n = static_cast<std::size_t>(CPU_COUNT(&set));
        // cgroup v2: "max 100000" or "<quota> <period>"; v1: two files.
        double quota = -1.0, period = -1.0;
        if (std::ifstream v2("/sys/fs/cgroup/cpu.max"); v2)
        {
            std::string q;
            v2 >> q >> period;
            if (q != "max")
                quota = std::stod(q);
        }
        else if (std::ifstream q1("/sys/fs/cgroup/cpu/cpu.cfs_quota_us"), p1("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
                 q1 && p1)
            q1 >> quota, p1 >> period;
        if (quota > 0.0 && period > 0.0)
            n = std::min(n, static_cast<std::size_t>(std::max(1.0, std::ceil(quota / period))));
#endif
        return n > 0 ? n : 1;
    }

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
