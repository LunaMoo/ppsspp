// SPDX-License-Identifier: see LICENSE
#pragma once
#include <cstddef>
#include <cstdint>

namespace ipbp {

class Sha1 {
public:
    Sha1() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void finish(uint8_t out[20]);
    static void hash(const void* data, size_t len, uint8_t out[20]);

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint64_t len_;
    uint8_t buf_[64];
    size_t used_;
};

// HMAC-SHA1 (used for deterministic ECDSA nonces).
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]);

}  // namespace ipbp
