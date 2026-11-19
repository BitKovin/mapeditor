// base/jobs -- see jobs.h.
#include "base/jobs.h"

#include <algorithm>
#include <exception>
#include <string>

#include "base/log.h"

namespace native::base {

int JobPool::defaultThreadCount() {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(1, hw - 3);
}

JobPool::JobPool(int threads) {
    const int n = threads > 0 ? threads : defaultThreadCount();
    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
}

JobPool::~JobPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& w : workers_) w.join();
}

void JobPool::submit(Task task, int priority, VersionToken token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(Item{priority, nextSeq_++, std::move(task), std::move(token)});
    }
    wake_.notify_one();
}

void JobPool::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

size_t JobPool::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void JobPool::workerLoop() {
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            item = queue_.top();
            queue_.pop();
            ++running_;
        }
        if (item.token.cancelled()) {
            skipped_.fetch_add(1);
        } else {
            try {
                item.task();
            } catch (const std::exception& e) {
                failed_.fetch_add(1);
                logError(std::string("job threw: ") + e.what());
            } catch (...) {
                failed_.fetch_add(1);
                logError("job threw a non-standard exception");
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
            if (queue_.empty() && running_ == 0) idle_.notify_all();
        }
    }
}

} // namespace native::base
