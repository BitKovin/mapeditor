// base/jobs -- the job pool: a small prioritised task queue on fixed worker threads.
//
// Owns: CPU work scheduling for derived data (CSG, surfaces, layout, texel samples, BLAS input), 01_ARCHITECTURE.md §2.
// Thread: submit() / cancel from any thread; tasks run on the pool's workers. Tasks must not touch Qt or Vulkan objects.
// Spec: 01_ARCHITECTURE.md §2 and §7 (no exception leaves a task: it is caught, logged and counted), 08 T1.1.
//
//  - Workers: hardware threads - 3, at least 1 (the UI, render and bake threads keep their cores).
//  - Priority: larger number runs first; equal priorities run in submission order.
//  - Cancellation by version: a VersionCounter is bumped whenever the work a task was made for is superseded; a task
//    submitted with an older VersionToken is skipped when a worker picks it up, and a running task can poll
//    token.cancelled() to stop early. ("Latest wins", 01 §3.)
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace native::base {

// Snapshot of a VersionCounter at the moment the task was created.
class VersionToken {
public:
    VersionToken() = default;                       // an empty token is never cancelled
    bool cancelled() const { return counter_ && counter_->load(std::memory_order_acquire) != value_; }

private:
    friend class VersionCounter;
    std::shared_ptr<std::atomic<uint64_t>> counter_;
    uint64_t value_ = 0;
};

class VersionCounter {
public:
    VersionCounter() : counter_(std::make_shared<std::atomic<uint64_t>>(0)) {}
    // Cancels every task made with an earlier token; returns the new version.
    uint64_t bump() { return counter_->fetch_add(1, std::memory_order_acq_rel) + 1; }
    uint64_t version() const { return counter_->load(std::memory_order_acquire); }
    VersionToken token() const {
        VersionToken t;
        t.counter_ = counter_;
        t.value_ = counter_->load(std::memory_order_acquire);
        return t;
    }

private:
    std::shared_ptr<std::atomic<uint64_t>> counter_;
};

class JobPool {
public:
    using Task = std::function<void()>;

    // threads <= 0: defaultThreadCount().
    explicit JobPool(int threads = 0);
    ~JobPool();                                      // finishes nothing new: queued tasks are dropped, running ones complete
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    static int defaultThreadCount();

    void submit(Task task, int priority = 0, VersionToken token = {});

    // Blocks until the queue is empty and no task is running. For tests and shutdown, never on the UI thread.
    void waitIdle();

    size_t pending() const;                          // queued, not yet started
    int threadCount() const { return static_cast<int>(workers_.size()); }
    uint64_t skippedCount() const { return skipped_.load(); }   // tasks dropped because their token was cancelled
    uint64_t failedCount() const { return failed_.load(); }     // tasks that threw

private:
    struct Item {
        int priority;
        uint64_t seq;
        Task task;
        VersionToken token;
    };
    struct Order {
        bool operator()(const Item& a, const Item& b) const {   // true = a after b
            return a.priority != b.priority ? a.priority < b.priority : a.seq > b.seq;
        }
    };

    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::priority_queue<Item, std::vector<Item>, Order> queue_;
    std::vector<std::thread> workers_;
    uint64_t nextSeq_ = 0;
    int running_ = 0;
    bool stopping_ = false;
    std::atomic<uint64_t> skipped_{0}, failed_{0};
};

} // namespace native::base
