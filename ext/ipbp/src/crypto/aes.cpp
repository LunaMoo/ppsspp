// SPDX-License-Identifier: see LICENSE
#include "aes.h"

#include <cstring>

namespace ipbp {
namespace {

struct Tables {
    uint8_t sbox[256], inv[256];
    uint32_t te[4][256], td[4][256];

    static uint8_t xt(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = xt(a);
            b >>= 1;
        }
        return r;
    }

    Tables() {
        // Build S-box from multiplicative inverse + affine transform.
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            q ^= q << 1;
            q ^= q << 2;
            q ^= q << 4;
            if (q & 0x80) q ^= 0x09;
            uint8_t x = static_cast<uint8_t>(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^
                                             (q << 4 | q >> 4));
            sbox[p] = static_cast<uint8_t>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = static_cast<uint8_t>(i);
        for (int i = 0; i < 256; i++) {
            uint8_t s = sbox[i];
            uint32_t t = (uint32_t)mul(s, 2) | (uint32_t)s << 8 | (uint32_t)s << 16 | (uint32_t)mul(s, 3) << 24;
            uint8_t v = inv[i];
            uint32_t d = (uint32_t)mul(v, 14) | (uint32_t)mul(v, 9) << 8 | (uint32_t)mul(v, 13) << 16 |
                         (uint32_t)mul(v, 11) << 24;
            for (int r = 0; r < 4; r++) {
                te[r][i] = (t << (8 * r)) | (r ? (t >> (32 - 8 * r)) : 0);
                td[r][i] = (d << (8 * r)) | (r ? (d >> (32 - 8 * r)) : 0);
            }
        }
    }
};

const Tables& T() {
    static const Tables t;
    return t;
}

inline uint32_t load32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline void store32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
inline uint32_t subWord(uint32_t w) {
    const auto& t = T();
    return (uint32_t)t.sbox[w & 0xff] | (uint32_t)t.sbox[(w >> 8) & 0xff] << 8 |
           (uint32_t)t.sbox[(w >> 16) & 0xff] << 16 | (uint32_t)t.sbox[w >> 24] << 24;
}

}  // namespace

// Column words are little-endian (byte 0 of the column in the low bits).
void Aes128::setKey(const uint8_t key[16]) {
    const auto& t = T();
    static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36};
    for (int i = 0; i < 4; i++) rk_[i] = load32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t w = rk_[i - 1];
        if (i % 4 == 0) w = subWord((w >> 8) | (w << 24)) ^ rcon[i / 4 - 1];
        rk_[i] = rk_[i - 4] ^ w;
    }
    // Decryption keys: reverse round order, InvMixColumns on inner rounds.
    for (int r = 0; r <= 10; r++) {
        for (int c = 0; c < 4; c++) {
            uint32_t w = rk_[(10 - r) * 4 + c];
            if (r > 0 && r < 10) {
                w = t.td[0][t.sbox[w & 0xff]] ^ t.td[1][t.sbox[(w >> 8) & 0xff]] ^
                    t.td[2][t.sbox[(w >> 16) & 0xff]] ^ t.td[3][t.sbox[w >> 24]];
            }
            drk_[r * 4 + c] = w;
        }
    }
}

void Aes128::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const auto& t = T();
    uint32_t s0 = load32(in) ^ rk_[0], s1 = load32(in + 4) ^ rk_[1], s2 = load32(in + 8) ^ rk_[2],
             s3 = load32(in + 12) ^ rk_[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t* k = rk_ + 4 * r;
        uint32_t t0 = t.te[0][s0 & 0xff] ^ t.te[1][(s1 >> 8) & 0xff] ^ t.te[2][(s2 >> 16) & 0xff] ^ t.te[3][s3 >> 24] ^ k[0];
        uint32_t t1 = t.te[0][s1 & 0xff] ^ t.te[1][(s2 >> 8) & 0xff] ^ t.te[2][(s3 >> 16) & 0xff] ^ t.te[3][s0 >> 24] ^ k[1];
        uint32_t t2 = t.te[0][s2 & 0xff] ^ t.te[1][(s3 >> 8) & 0xff] ^ t.te[2][(s0 >> 16) & 0xff] ^ t.te[3][s1 >> 24] ^ k[2];
        uint32_t t3 = t.te[0][s3 & 0xff] ^ t.te[1][(s0 >> 8) & 0xff] ^ t.te[2][(s1 >> 16) & 0xff] ^ t.te[3][s2 >> 24] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint8_t* S = t.sbox;
    const uint32_t* k = rk_ + 40;
    store32(out, ((uint32_t)S[s0 & 0xff] | (uint32_t)S[(s1 >> 8) & 0xff] << 8 | (uint32_t)S[(s2 >> 16) & 0xff] << 16 | (uint32_t)S[s3 >> 24] << 24) ^ k[0]);
    store32(out + 4, ((uint32_t)S[s1 & 0xff] | (uint32_t)S[(s2 >> 8) & 0xff] << 8 | (uint32_t)S[(s3 >> 16) & 0xff] << 16 | (uint32_t)S[s0 >> 24] << 24) ^ k[1]);
    store32(out + 8, ((uint32_t)S[s2 & 0xff] | (uint32_t)S[(s3 >> 8) & 0xff] << 8 | (uint32_t)S[(s0 >> 16) & 0xff] << 16 | (uint32_t)S[s1 >> 24] << 24) ^ k[2]);
    store32(out + 12, ((uint32_t)S[s3 & 0xff] | (uint32_t)S[(s0 >> 8) & 0xff] << 8 | (uint32_t)S[(s1 >> 16) & 0xff] << 16 | (uint32_t)S[s2 >> 24] << 24) ^ k[3]);
}

void Aes128::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const auto& t = T();
    uint32_t s0 = load32(in) ^ drk_[0], s1 = load32(in + 4) ^ drk_[1], s2 = load32(in + 8) ^ drk_[2],
             s3 = load32(in + 12) ^ drk_[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t* k = drk_ + 4 * r;
        uint32_t t0 = t.td[0][s0 & 0xff] ^ t.td[1][(s3 >> 8) & 0xff] ^ t.td[2][(s2 >> 16) & 0xff] ^ t.td[3][s1 >> 24] ^ k[0];
        uint32_t t1 = t.td[0][s1 & 0xff] ^ t.td[1][(s0 >> 8) & 0xff] ^ t.td[2][(s3 >> 16) & 0xff] ^ t.td[3][s2 >> 24] ^ k[1];
        uint32_t t2 = t.td[0][s2 & 0xff] ^ t.td[1][(s1 >> 8) & 0xff] ^ t.td[2][(s0 >> 16) & 0xff] ^ t.td[3][s3 >> 24] ^ k[2];
        uint32_t t3 = t.td[0][s3 & 0xff] ^ t.td[1][(s2 >> 8) & 0xff] ^ t.td[2][(s1 >> 16) & 0xff] ^ t.td[3][s0 >> 24] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint8_t* I = t.inv;
    const uint32_t* k = drk_ + 40;
    store32(out, ((uint32_t)I[s0 & 0xff] | (uint32_t)I[(s3 >> 8) & 0xff] << 8 | (uint32_t)I[(s2 >> 16) & 0xff] << 16 | (uint32_t)I[s1 >> 24] << 24) ^ k[0]);
    store32(out + 4, ((uint32_t)I[s1 & 0xff] | (uint32_t)I[(s0 >> 8) & 0xff] << 8 | (uint32_t)I[(s3 >> 16) & 0xff] << 16 | (uint32_t)I[s2 >> 24] << 24) ^ k[1]);
    store32(out + 8, ((uint32_t)I[s2 & 0xff] | (uint32_t)I[(s1 >> 8) & 0xff] << 8 | (uint32_t)I[(s0 >> 16) & 0xff] << 16 | (uint32_t)I[s3 >> 24] << 24) ^ k[2]);
    store32(out + 12, ((uint32_t)I[s3 & 0xff] | (uint32_t)I[(s2 >> 8) & 0xff] << 8 | (uint32_t)I[(s1 >> 16) & 0xff] << 16 | (uint32_t)I[s0 >> 24] << 24) ^ k[3]);
}

}  // namespace ipbp
