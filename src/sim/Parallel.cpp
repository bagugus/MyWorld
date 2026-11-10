#include "sim/Parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define LW_SPIN_PAUSE() _mm_pause()
#else
#define LW_SPIN_PAUSE() std::this_thread::yield()
#endif

namespace lw {

namespace {

// Сколько рабочий ждёт следующего прохода «на ходу», прежде чем уснуть.
std::chrono::microseconds spinTime() {
    static const std::chrono::microseconds value{[] {
        const char* env = std::getenv("LW_SPIN_US");
        return env != nullptr ? std::atoll(env) : 20LL;
    }()};
    return value;
}

int defaultThreads() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(hardware == 0 ? 1 : hardware), 1, kMaxParallelThreads);
}

using Task = std::function<void(std::size_t, std::size_t, int)>;

class Pool {
public:
    static Pool& instance() {
        static Pool pool;
        return pool;
    }

    ~Pool() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    int threads() const { return threads_; }
    void setThreads(int threads) { threads_ = std::clamp(threads, 1, kMaxParallelThreads); }

    void run(std::size_t count, std::size_t chunk, const Task& fn) {
        const std::size_t chunks = (count + chunk - 1) / chunk;
        const int helpers = std::min(threads_ - 1, static_cast<int>(chunks) - 1);
        if (helpers <= 0) {
            fn(0, count, 0);
            return;
        }
        // Проход у пула один: второй поток (мир грузится в фоне, этап С.2)
        // ждёт, пока кончится чужой. Сами куски от этого не меняются.
        const std::lock_guard runLock(runMutex_);
        ensureWorkers(helpers);
        std::uint32_t generation = 0;
        {
            const std::lock_guard lock(mutex_);
            generation = ++generation_;
            job_ = Job{&fn, count, chunk, chunks, helpers};
            done_.store(0, std::memory_order_relaxed);
            // Номер прохода — в старших битах счётчика кусков: опоздавший
            // рабочий с прошлого прохода не возьмёт кусок нового.
            ticket_.store(static_cast<std::uint64_t>(generation) << 32, std::memory_order_release);
        }
        if (sleeping_.load(std::memory_order_acquire) > 0) {
            wake_.notify_all();
        }
        work(generation, job_, 0);
        while (done_.load(std::memory_order_acquire) < chunks) {
            LW_SPIN_PAUSE();
        }
    }

private:
    struct Job {
        const Task* fn = nullptr;
        std::size_t count = 0;
        std::size_t chunk = 1;
        std::size_t chunks = 0;
        int helpers = 0;
    };

    void ensureWorkers(int count) {
        while (static_cast<int>(workers_.size()) < count) {
            const int index = static_cast<int>(workers_.size()) + 1;
            workers_.emplace_back([this, index] { loop(index); });
        }
    }

    // Взять и сделать куски прохода generation, пока они есть.
    void work(std::uint32_t generation, const Job& job, int worker) {
        for (;;) {
            std::uint64_t ticket = ticket_.load(std::memory_order_acquire);
            std::size_t index = 0;
            for (;;) {
                if (static_cast<std::uint32_t>(ticket >> 32) != generation) {
                    return;
                }
                index = static_cast<std::size_t>(ticket & 0xFFFFFFFFu);
                if (index >= job.chunks) {
                    return;
                }
                if (ticket_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel)) {
                    break;
                }
            }
            const std::size_t begin = index * job.chunk;
            (*job.fn)(begin, std::min(job.count, begin + job.chunk), worker);
            done_.fetch_add(1, std::memory_order_release);
        }
    }

    void loop(int index) {
        std::uint32_t seen = 0;
        for (;;) {
            // Ждать нового прохода: сначала на ходу, потом во сне.
            const auto spinUntil = std::chrono::steady_clock::now() + spinTime();
            int spins = 0;
            while (published() == seen) {
                if ((++spins & 63) == 0 && std::chrono::steady_clock::now() > spinUntil) {
                    std::unique_lock lock(mutex_);
                    sleeping_.fetch_add(1, std::memory_order_acq_rel);
                    wake_.wait(lock, [&] { return generation_ != seen; });
                    sleeping_.fetch_sub(1, std::memory_order_acq_rel);
                    break;
                }
                LW_SPIN_PAUSE();
            }
            Job job;
            std::uint32_t generation = 0;
            {
                const std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                generation = generation_;
                job = job_;
            }
            seen = generation;
            if (index <= job.helpers) {
                work(generation, job, index);
            }
        }
    }

    std::uint32_t published() const {
        return static_cast<std::uint32_t>(ticket_.load(std::memory_order_acquire) >> 32);
    }

    int threads_ = defaultThreads();
    std::mutex runMutex_; // один проход за раз, из какого бы потока ни звали
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::thread> workers_;
    std::uint32_t generation_ = 0; // под mutex_
    bool stopping_ = false;        // под mutex_
    Job job_;                      // под mutex_ (и у вызывающего — пока идёт проход)
    std::atomic<std::uint64_t> ticket_{0};
    std::atomic<std::size_t> done_{0};
    std::atomic<int> sleeping_{0};
};

} // namespace

void setParallelThreads(int threads) {
    Pool::instance().setThreads(threads);
}

int parallelThreads() {
    return Pool::instance().threads();
}

void parallelFor(std::size_t count, std::size_t chunk, std::size_t minParallel,
                 const std::function<void(std::size_t, std::size_t, int)>& fn) {
    if (count == 0) {
        return;
    }
    chunk = std::max<std::size_t>(chunk, 1);
    if (count < minParallel || Pool::instance().threads() <= 1) {
        fn(0, count, 0);
        return;
    }
    Pool::instance().run(count, chunk, fn);
}

} // namespace lw
