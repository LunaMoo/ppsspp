// SPDX-License-Identifier: see LICENSE
// PSP "amctrl" DRM primitives (sceDrmBBMac*, sceDrmBBCipher*, sceNpDrmGetFixedKey),
// re-implemented on top of plain AES using the KIRK engine's key-seed keys.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ipbp {
namespace psp {

// KIRK commands 4/7 (AES-128-CBC, zero IV) with a key selected by seed.
void kirkEncrypt(uint8_t seed, uint8_t* data, size_t size);  // CMD4 (in place)
void kirkDecrypt(uint8_t seed, uint8_t* data, size_t size);  // CMD7 (in place)

// sceDrmBBMac: CBC-MAC/CMAC hybrid. Only types 1 and 3 are supported (type 2 needs console fuses).
class BBMac {
public:
    explicit BBMac(int type) : type_(type) {}
    void update(const uint8_t* data, size_t size);
    // Finalize. If vkey != nullptr the MAC is bound to that version key.
    void final(uint8_t out[16], const uint8_t* vkey);
    int type() const { return type_; }

private:
    void flushBlocks(const uint8_t* data, size_t size);
    int type_;
    uint8_t state_[16] = {};
    uint8_t pad_[16] = {};
    size_t padLen_ = 0;
};

// sceDrmBBMacFinal2 "build" step for type 3: mac = AES-enc(K63, mac).
void bbmacBuildFinal2(int type, uint8_t mac[16]);
// Recover the version key from a stored (final2) MAC: vkey = f(mac(data), stored).
void bbmacGetKey(BBMac& mac, const uint8_t storedMac[16], uint8_t vkey[16]);
// Verify a stored MAC against data hashed in `mac` with version key `vkey`.
bool bbmacVerify(BBMac& mac, const uint8_t storedMac[16], const uint8_t vkey[16]);

// sceDrmBBCipher, type 1, mode 2 (header-key driven). Encrypt == decrypt (XOR keystream).
class BBCipher {
public:
    // headerKey: 16 bytes; vkey: 16 bytes (may be null); seed: block counter base.
    BBCipher(const uint8_t headerKey[16], const uint8_t* vkey, uint32_t seed);
    void update(uint8_t* data, size_t size);

private:
    uint8_t key_[16];
    uint32_t seed_;
};

// sceNpDrmGetFixedKey: derive a version key from a content id (up to 0x30 chars) and NP flags.
bool npDrmGetFixedKey(const char* contentId, uint32_t npFlags, uint8_t vkey[16]);

}  // namespace psp
}  // namespace ipbp
