// SPDX-License-Identifier: see LICENSE
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipbp {
namespace detail {

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

std::string hex(const uint8_t* p, size_t n);
bool removeFile(const std::string& path);
bool renameReplace(const std::string& from, const std::string& to);

// Minimal fixed-size thread pool running "parallel for" jobs.
class WorkerPool {
public:
    explicit WorkerPool(unsigned threads);
    ~WorkerPool();
    // Runs fn(i) for i in [0,n) across the pool (and the calling thread); blocks until done.
    void run(size_t n, const std::function<void(size_t)>& fn);
    unsigned size() const { return (unsigned)workers_.size() + 1; }

private:
    void loop();
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, doneCv_;
    const std::function<void(size_t)>* job_ = nullptr;
    size_t n_ = 0;
    std::atomic<size_t> next_{0};
    size_t active_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace detail
}  // namespace ipbp
