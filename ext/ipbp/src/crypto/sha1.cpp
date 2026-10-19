// SPDX-License-Identifier: see LICENSE
#include "sha1.h"

#include <cstring>
#include <vector>

namespace ipbp {

static inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Sha1::reset() {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    len_ = 0;
    used_ = 0;
}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += len;
    while (len) {
        size_t n = 64 - used_;
        if (n > len) n = len;
        memcpy(buf_ + used_, p, n);
        used_ += n; p += n; len -= n;
        if (used_ == 64) { block(buf_); used_ = 0; }
    }
}

void Sha1::finish(uint8_t out[20]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (used_ != 56) update(&z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h_[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h_[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h_[i] >> 8);
        out[4 * i + 3] = (uint8_t)h_[i];
    }
    reset();
}

void Sha1::hash(const void* data, size_t len, uint8_t out[20]) {
    Sha1 s;
    s.update(data, len);
    s.finish(out);
}

void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) Sha1::hash(key, keyLen, k);
    else memcpy(k, key, keyLen);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    uint8_t inner[20];
    Sha1 s;
    s.update(ip, 64); s.update(msg, msgLen); s.finish(inner);
    s.update(op, 64); s.update(inner, 20); s.finish(out);
}

}  // namespace ipbp
