#include "core/parallel.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace core {

namespace {

thread_local bool tlsInsideRange = false;

unsigned defaultHelpers() {
    if (const char* env = std::getenv("MAPEDITOR_THREADS")) {
        const int total = std::atoi(env);
        if (total >= 1) return static_cast<unsigned>(total - 1);
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 1 ? hc - 1 : 0;
}

class Pool {
public:
    ~Pool() { stop(); }

    void setHelpers(unsigned h) {
        std::lock_guard<std::mutex> call(callMutex_);  // not while a parallelFor runs
        stop();
        wanted_ = h;
        configured_ = true;
    }
    unsigned helpers() {
        std::lock_guard<std::mutex> call(callMutex_);
        return currentHelpers();
    }

    void run(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn) {
        if (n == 0) return;
        grain = std::max<size_t>(1, grain);
        if (tlsInsideRange) { inlineRun(n, grain, fn); return; }
        std::unique_lock<std::mutex> call(callMutex_);
        const unsigned helpers = currentHelpers();
        const size_t ranges = (n + grain - 1) / grain;
        if (helpers == 0 || ranges <= 1) {
            tlsInsideRange = true;
            try { inlineRun(n, grain, fn); } catch (...) { tlsInsideRange = false; throw; }
            tlsInsideRange = false;
            return;
        }
        ensureThreads(helpers);
        Job job;
        job.fn = &fn;
        job.n = n;
        job.grain = grain;
        job.pending.store(static_cast<int>(threads_.size()));
        {
            std::lock_guard<std::mutex> lock(m_);
            job_ = &job;
            ++generation_;
        }
        cv_.notify_all();
        work(job);  // the caller takes part
        {
            std::unique_lock<std::mutex> lock(m_);
            done_.wait(lock, [&] { return job.pending.load() == 0; });
            job_ = nullptr;
        }
        if (job.error) std::rethrow_exception(job.error);
    }

private:
    static void inlineRun(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn) {
        for (size_t b = 0; b < n; b += grain) fn(b, std::min(n, b + grain));  // the same ranges the helpers would get
    }
    struct Job {
        const std::function<void(size_t, size_t)>* fn = nullptr;
        size_t n = 0, grain = 1;
        std::atomic<size_t> next{0};
        std::atomic<int> pending{0};  // helpers that have not finished their share yet
        std::mutex errorMutex;
        std::exception_ptr error;
    };

    unsigned currentHelpers() {
        if (!configured_) { wanted_ = defaultHelpers(); configured_ = true; }
        return wanted_;
    }

    void work(Job& job) {
        tlsInsideRange = true;
        for (;;) {
            const size_t begin = job.next.fetch_add(job.grain);
            if (begin >= job.n) break;
            try {
                (*job.fn)(begin, std::min(job.n, begin + job.grain));
            } catch (...) {
                std::lock_guard<std::mutex> lock(job.errorMutex);
                if (!job.error) job.error = std::current_exception();
                job.next.store(job.n);  // abandon the remaining ranges
            }
        }
        tlsInsideRange = false;
    }

    void ensureThreads(unsigned helpers) {
        if (threads_.size() == helpers) return;
        stop();
        quit_ = false;
        for (unsigned i = 0; i < helpers; ++i) threads_.emplace_back([this] { loop(); });
    }

    void loop() {
        uint64_t seen = 0;
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
                job = job_;
            }
            if (!job) continue;
            work(*job);
            {
                std::lock_guard<std::mutex> lock(m_);
                job->pending.fetch_sub(1);
            }
            done_.notify_all();
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
    }

    std::mutex callMutex_;  // one parallelFor at a time
    std::mutex m_;
    std::condition_variable cv_, done_;
    std::vector<std::thread> threads_;
    Job* job_ = nullptr;
    uint64_t generation_ = 0;
    bool quit_ = false, configured_ = false;
    unsigned wanted_ = 0;
};

Pool& pool() {
    static Pool p;
    return p;
}

}  // namespace

unsigned parallelHelpers() { return pool().helpers(); }
void setParallelHelpers(unsigned helpers) { pool().setHelpers(helpers); }
void parallelFor(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn) { pool().run(n, grain, fn); }

}  // namespace core
