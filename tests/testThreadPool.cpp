#include <gtest/gtest.h>

#include "quantModeling/utils/concurrent_queue.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace quantModeling
{

    // ── ConcurrentQueue ──────────────────────────────────────────────────────

    TEST(ConcurrentQueue, PushThenTryPopIsFIFO)
    {
        ConcurrentQueue<int> q;
        q.push(1);
        q.push(2);
        q.push(3);

        int v = 0;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, 1);
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, 2);
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, 3);
    }

    TEST(ConcurrentQueue, TryPopOnEmptyReturnsFalse)
    {
        ConcurrentQueue<int> q;
        int v = 0;
        EXPECT_FALSE(q.try_pop(v));
    }

    TEST(ConcurrentQueue, InterruptWakesABlockedPopWithoutAnItem)
    {
        ConcurrentQueue<int> q;
        int v = 42;
        std::thread waiter([&]
                           { q.pop(v); });

        // Give the waiter a moment to actually block in pop() before
        // interrupting -- a race that would just make this test slower on
        // an unlucky schedule, never flaky in the wrong direction (interrupt
        // always wakes every future wait too).
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.interrupt();
        waiter.join();

        EXPECT_EQ(v, 42); // untouched: nothing was ever pushed
    }

    TEST(ConcurrentQueue, PopReturnsAPushedItemEvenWhenBlockedFirst)
    {
        ConcurrentQueue<int> q;
        int v = 0;
        std::thread waiter([&]
                           { q.pop(v); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.push(7);
        waiter.join();
        EXPECT_EQ(v, 7);
    }

    // ── ThreadPool ───────────────────────────────────────────────────────────

    TEST(ThreadPool, StartCreatesTheRequestedThreadCount)
    {
        ThreadPool pool;
        pool.start(3);
        EXPECT_EQ(pool.num_threads(), 3u);
        pool.stop();
        EXPECT_EQ(pool.num_threads(), 0u);
    }

    TEST(ThreadPool, SpawnTaskReturnsTheComputedResult)
    {
        ThreadPool pool;
        pool.start(2);
        auto handle = pool.spawn_task([]
                                      { return true; });
        EXPECT_TRUE(pool.active_wait(handle));
        pool.stop();
    }

    TEST(ThreadPool, ActiveWaitDrainsTheQueueWithZeroWorkerThreads)
    {
        // start(0): the pool has no workers of its own, so active_wait on
        // the calling thread must do all the work itself -- the point of
        // the design (no core sits idle, and this is also the degenerate
        // "no thread pool at all" case simulate_parallel_aad should reduce
        // to safely).
        ThreadPool pool;
        pool.start(0);
        EXPECT_EQ(pool.num_threads(), 0u);

        std::atomic<int> counter{0};
        std::vector<TaskHandle> handles;
        for (int i = 0; i < 10; ++i)
            handles.push_back(pool.spawn_task([&counter]
                                              {
                ++counter;
                return true; }));
        for (auto &h : handles)
            EXPECT_TRUE(pool.active_wait(h));

        EXPECT_EQ(counter.load(), 10);
    }

    TEST(ThreadPool, ThreadNumIsZeroOnTheCallingThread)
    {
        // "0 = the main thread" (blueprint §8.1) -- true for any thread the
        // pool itself did not start, this test's own thread included.
        EXPECT_EQ(ThreadPool::thread_num(), 0u);
    }

    TEST(ThreadPool, WorkerThreadsReportDistinctNonZeroThreadNum)
    {
        // Note: 0 (the main-thread sentinel) is itself a *legitimate* value
        // to see here, not a bug -- active_wait()'s whole point is that the
        // calling thread also drains the queue instead of sleeping, so a
        // task can genuinely run on the main thread if it wins the race
        // against a worker's own try_pop. What must never happen is a value
        // outside [0, n_threads], and more than one distinct *worker* id
        // must appear -- otherwise the pool never actually dispatched in
        // parallel.
        //
        // Each task holds its thread until two workers are inside a task at
        // once. Counting ids over tasks that return at once would depend on
        // the scheduler: on a loaded machine one thread drains the whole
        // queue before the others wake up. The deadline is shared, so a pool
        // that cannot run two tasks at once fails instead of hanging.
        constexpr std::size_t n_threads = 4;
        constexpr int n_tasks = 40; // several tasks per thread
        ThreadPool pool;
        pool.start(n_threads);

        std::mutex mtx;
        std::condition_variable two_workers_in;
        std::set<std::size_t> seen;
        std::size_t distinct_workers = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::vector<TaskHandle> handles;
        for (int i = 0; i < n_tasks; ++i)
        {
            handles.push_back(pool.spawn_task([&]
                                              {
                const std::size_t n = ThreadPool::thread_num();
                std::unique_lock<std::mutex> lock(mtx);
                if (seen.insert(n).second && n >= 1)
                    ++distinct_workers;
                two_workers_in.notify_all();
                two_workers_in.wait_until(lock, deadline, [&]
                                          { return distinct_workers > 1; });
                return true; }));
        }
        for (auto &h : handles)
            EXPECT_TRUE(pool.active_wait(h));

        for (std::size_t n : seen)
            EXPECT_LE(n, n_threads);
        EXPECT_GT(distinct_workers, 1u);

        pool.stop();
    }

    TEST(ThreadPool, StopThenStartAgainWorks)
    {
        ThreadPool pool;
        pool.start(2);
        auto h1 = pool.spawn_task([]
                                  { return true; });
        EXPECT_TRUE(pool.active_wait(h1));
        pool.stop();

        pool.start(2);
        auto h2 = pool.spawn_task([]
                                  { return true; });
        EXPECT_TRUE(pool.active_wait(h2));
        pool.stop();
    }

} // namespace quantModeling
