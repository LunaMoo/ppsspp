// SPDX-License-Identifier: see LICENSE
// ECDSA over the 160-bit curve used by the PSP KIRK engine (commands 0x10/0x11).
#pragma once
#include <cstdint>

namespace ipbp {
namespace ecdsa {

// Sign a 20-byte SHA-1 digest with a 20-byte big-endian private key.
// The nonce is derived deterministically from (key, hash, nonceSeed) so output is reproducible.
// Output: r(20) || s(20), big-endian.
void sign(const uint8_t priv[20], const uint8_t hash[20], uint8_t sig[40], uint32_t nonceSeed = 0);

// Verify a signature against a public key Qx(20)||Qy(20).
bool verify(const uint8_t pub[40], const uint8_t hash[20], const uint8_t sig[40]);

// Compute the public key for a private key.
void publicKey(const uint8_t priv[20], uint8_t pub[40]);

}  // namespace ecdsa
}  // namespace ipbp
