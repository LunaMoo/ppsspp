// SPDX-License-Identifier: see LICENSE
// Minimal portable AES-128 (encrypt + decrypt, single block).
#pragma once
#include <cstdint>

namespace ipbp {

class Aes128 {
public:
    Aes128() = default;
    explicit Aes128(const uint8_t key[16]) { setKey(key); }
    void setKey(const uint8_t key[16]);
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t rk_[44] = {};   // encryption round keys
    uint32_t drk_[44] = {};  // decryption round keys (equivalent inverse cipher)
};

}  // namespace ipbp
