// SPDX-License-Identifier: see LICENSE
#include "amctrl.h"

#include <cstring>

#include "../crypto/aes.h"

namespace ipbp {
namespace psp {
namespace {

// KIRK AES keys selected by key seed (from the KIRK engine key table).
struct SeedKey {
    uint8_t seed;
    uint8_t key[16];
};
const SeedKey kSeedKeys[] = {
    {0x38, {0x12, 0x46, 0x8D, 0x7E, 0x1C, 0x42, 0x20, 0x9B, 0xBA, 0x54, 0x26, 0x83, 0x5E, 0xB0, 0x33, 0x03}},
    {0x39, {0xC4, 0x3B, 0xB6, 0xD6, 0x53, 0xEE, 0x67, 0x49, 0x3E, 0xA9, 0x5F, 0xBC, 0x0C, 0xED, 0x6F, 0x8A}},
    {0x3A, {0x2C, 0xC3, 0xCF, 0x8C, 0x28, 0x78, 0xA5, 0xA6, 0x63, 0xE2, 0xAF, 0x2D, 0x71, 0x5E, 0x86, 0xBA}},
    {0x63, {0x9C, 0x9B, 0x13, 0x72, 0xF8, 0xC6, 0x40, 0xCF, 0x1C, 0x62, 0xF5, 0xD5, 0x92, 0xDD, 0xB5, 0x82}},
};

// amctrl constants
const uint8_t kMacXor[16] = {0xE3, 0x50, 0xED, 0x1D, 0x91, 0x0A, 0x1F, 0xD0, 0x29, 0xBB, 0x1C, 0x3E, 0xF3, 0x40, 0x77, 0xFB};
const uint8_t kCipherPost[16] = {0x13, 0x5F, 0xA4, 0x7C, 0xAB, 0x39, 0x5B, 0xA4, 0x76, 0xB8, 0xCC, 0xA9, 0x8F, 0x3A, 0x04, 0x45};
const uint8_t kCipherPre[16] = {0x67, 0x8D, 0x7F, 0xA3, 0x2A, 0x9C, 0xA0, 0xD1, 0x50, 0x8A, 0xD8, 0x38, 0x5E, 0x4B, 0x01, 0x7E};

// sceNpDrmGetFixedKey keys: [0] = fixed MAC key, [1..3] = per-type AES keys.
const uint8_t kNpDrmKeys[4][16] = {
    {0x38, 0x20, 0xD0, 0x11, 0x07, 0xA3, 0xFF, 0x3E, 0x0A, 0x4C, 0x20, 0x85, 0x39, 0x10, 0xB5, 0x54},
    {0x07, 0x3D, 0x9E, 0x9D, 0xA8, 0xFD, 0x3B, 0x2F, 0x63, 0x18, 0x93, 0x2E, 0xF8, 0x57, 0xA6, 0x64},
    {0x37, 0x49, 0xB7, 0x01, 0xCA, 0xE2, 0xE0, 0xC5, 0x44, 0x2E, 0x06, 0xB6, 0x1E, 0xFF, 0x84, 0xF2},
    {0x9D, 0x31, 0xB8, 0x5A, 0xC8, 0xFA, 0x16, 0x80, 0x73, 0x60, 0x18, 0x82, 0x18, 0x77, 0x91, 0x9D},
};

const Aes128& seedAes(uint8_t seed) {
    static const Aes128 aes[4] = {Aes128(kSeedKeys[0].key), Aes128(kSeedKeys[1].key), Aes128(kSeedKeys[2].key),
                                  Aes128(kSeedKeys[3].key)};
    for (int i = 0; i < 4; i++)
        if (kSeedKeys[i].seed == seed) return aes[i];
    return aes[0];  // not reached for supported seeds
}

inline void xor16(uint8_t* d, const uint8_t* s) {
    for (int i = 0; i < 16; i++) d[i] ^= s[i];
}

inline uint8_t macSeed(int type) { return type == 2 ? 0x3A : 0x38; }

void cmacDouble(uint8_t b[16]) {
    uint8_t carry = (b[0] & 0x80) ? 0x87 : 0;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    b[15] = (uint8_t)((b[15] << 1) ^ carry);
}

}  // namespace

void kirkEncrypt(uint8_t seed, uint8_t* data, size_t size) {
    const Aes128& aes = seedAes(seed);
    uint8_t iv[16] = {0};
    for (size_t i = 0; i + 16 <= size; i += 16) {
        xor16(data + i, iv);
        aes.encryptBlock(data + i, data + i);
        memcpy(iv, data + i, 16);
    }
}

void kirkDecrypt(uint8_t seed, uint8_t* data, size_t size) {
    const Aes128& aes = seedAes(seed);
    uint8_t iv[16] = {0}, c[16];
    for (size_t i = 0; i + 16 <= size; i += 16) {
        memcpy(c, data + i, 16);
        aes.decryptBlock(data + i, data + i);
        xor16(data + i, iv);
        memcpy(iv, c, 16);
    }
}

// ---------------------------------------------------------------- BBMac

void BBMac::flushBlocks(const uint8_t* data, size_t size) {
    const Aes128& aes = seedAes(macSeed(type_));
    for (size_t i = 0; i < size; i += 16) {
        xor16(state_, data + i);
        aes.encryptBlock(state_, state_);
    }
}

void BBMac::update(const uint8_t* data, size_t size) {
    if (padLen_ + size <= 16) {
        memcpy(pad_ + padLen_, data, size);
        padLen_ += size;
        return;
    }
    // Everything except the final 1..16 bytes is CBC-MACed now.
    size_t total = padLen_ + size;
    size_t keep = total & 15;
    if (keep == 0) keep = 16;
    size_t process = total - keep;  // multiple of 16
    // first block may combine pad + data
    uint8_t blk[16];
    size_t consumed = 0;  // bytes of `data` consumed
    size_t done = 0;
    if (padLen_) {
        memcpy(blk, pad_, padLen_);
        size_t need = 16 - padLen_;
        memcpy(blk + padLen_, data, need);
        flushBlocks(blk, 16);
        consumed = need;
        done = 16;
    }
    if (process > done) {
        flushBlocks(data + consumed, process - done);
        consumed += process - done;
    }
    memcpy(pad_, data + consumed, keep);
    padLen_ = keep;
}

void BBMac::final(uint8_t out[16], const uint8_t* vkey) {
    const Aes128& aes = seedAes(macSeed(type_));
    uint8_t k[16] = {0};
    aes.encryptBlock(k, k);
    cmacDouble(k);
    uint8_t last[16];
    memcpy(last, pad_, padLen_);
    if (padLen_ < 16) {
        cmacDouble(k);
        last[padLen_] = 0x80;
        memset(last + padLen_ + 1, 0, 15 - padLen_);
    }
    xor16(last, k);
    xor16(last, state_);
    aes.encryptBlock(last, last);
    xor16(last, kMacXor);
    if (vkey) {
        xor16(last, vkey);
        aes.encryptBlock(last, last);
    }
    memcpy(out, last, 16);
    memset(state_, 0, 16);
    padLen_ = 0;
}

void bbmacBuildFinal2(int type, uint8_t mac[16]) {
    if (type == 3) seedAes(0x63).encryptBlock(mac, mac);
}

void bbmacGetKey(BBMac& mac, const uint8_t storedMac[16], uint8_t vkey[16]) {
    int type = mac.type();
    uint8_t tmp[16], t[16];
    mac.final(tmp, nullptr);
    memcpy(t, storedMac, 16);
    if (type == 3) seedAes(0x63).decryptBlock(t, t);
    seedAes(macSeed(type)).decryptBlock(t, t);
    for (int i = 0; i < 16; i++) vkey[i] = tmp[i] ^ t[i];
}

bool bbmacVerify(BBMac& mac, const uint8_t storedMac[16], const uint8_t vkey[16]) {
    uint8_t m[16];
    mac.final(m, vkey);
    bbmacBuildFinal2(mac.type(), m);
    return memcmp(m, storedMac, 16) == 0;
}

// ---------------------------------------------------------------- BBCipher

BBCipher::BBCipher(const uint8_t headerKey[16], const uint8_t* vkey, uint32_t seed) {
    uint8_t k[16];
    memcpy(k, headerKey, 16);
    if (vkey) xor16(k, vkey);
    // Per-chunk key derivation in the original is constant, so do it once.
    xor16(k, kCipherPre);
    seedAes(0x39).decryptBlock(k, k);
    xor16(k, kCipherPost);
    memcpy(key_, k, 16);
    seed_ = seed + 1;
}

void BBCipher::update(uint8_t* data, size_t size) {
    const Aes128& aes = seedAes(0x63);
    uint8_t prev[16], ctr[16], ks[16];
    if (seed_ == 1) {
        memset(prev, 0, 16);
    } else {
        memcpy(prev, key_, 12);
        uint32_t s = seed_ - 1;
        prev[12] = (uint8_t)s; prev[13] = (uint8_t)(s >> 8); prev[14] = (uint8_t)(s >> 16); prev[15] = (uint8_t)(s >> 24);
    }
    for (size_t off = 0; off < size; off += 16) {
        memcpy(ctr, key_, 12);
        uint32_t s = seed_++;
        ctr[12] = (uint8_t)s; ctr[13] = (uint8_t)(s >> 8); ctr[14] = (uint8_t)(s >> 16); ctr[15] = (uint8_t)(s >> 24);
        aes.decryptBlock(ctr, ks);
        xor16(ks, prev);
        size_t n = size - off < 16 ? size - off : 16;
        for (size_t i = 0; i < n; i++) data[off + i] ^= ks[i];
        memcpy(prev, ctr, 16);
    }
}

// ---------------------------------------------------------------- fixed key

bool npDrmGetFixedKey(const char* contentId, uint32_t npFlags, uint8_t vkey[16]) {
    if (!(npFlags & 0x1000000)) return false;
    uint8_t cid[0x30] = {0};
    for (int i = 0; i < 0x30 && contentId[i]; i++) cid[i] = (uint8_t)contentId[i];
    BBMac mac(1);
    mac.update(cid, sizeof(cid));
    mac.final(vkey, kNpDrmKeys[0]);
    uint32_t type = npFlags & 0xff;
    if (type == 0) return true;
    if (type > 3) return false;
    Aes128(kNpDrmKeys[type]).encryptBlock(vkey, vkey);
    return true;
}

}  // namespace psp
}  // namespace ipbp
