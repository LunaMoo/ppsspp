// SPDX-License-Identifier: see LICENSE
#include "util.h"

namespace ipbp {
namespace detail {

std::string hex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

WorkerPool::WorkerPool(unsigned threads) {
    if (threads == 0) threads = 1;
    for (unsigned i = 1; i < threads; i++) workers_.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void WorkerPool::loop() {
    uint64_t seen = 0;
    for (;;) {
        const std::function<void(size_t)>* job;
        size_t n;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            // Woken too late: that run() already finished without us. Its n_ and the shared
            // next_ may belong to a newer run by the time we'd use them, so skip it.
            if (!job_) continue;
            job = job_;
            n = n_;
            active_++;
        }
        for (size_t i; (i = next_.fetch_add(1)) < n;) (*job)(i);
        {
            std::lock_guard<std::mutex> l(m_);
            if (--active_ == 0) doneCv_.notify_all();
        }
    }
}

void WorkerPool::run(size_t n, const std::function<void(size_t)>& fn) {
    if (workers_.empty() || n <= 1) {
        for (size_t i = 0; i < n; i++) fn(i);
        return;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        job_ = &fn;
        n_ = n;
        next_.store(0);
        generation_++;
    }
    cv_.notify_all();
    for (size_t i; (i = next_.fetch_add(1)) < n;) fn(i);
    std::unique_lock<std::mutex> l(m_);
    // Wait until every worker that picked up this generation has finished.
    doneCv_.wait(l, [&] { return active_ == 0 && next_.load() >= n; });
    job_ = nullptr;
}

}  // namespace detail
}  // namespace ipbp
