// SPDX-License-Identifier: see LICENSE
#include "ecdsa.h"

#include <cstring>

#include "sha1.h"

namespace ipbp {
namespace ecdsa {
namespace {

// 192-bit little-endian limb numbers (only 160 bits used for values; headroom for carries).
constexpr int L = 6;
struct Bn {
    uint32_t w[L] = {};
};

Bn fromBytes(const uint8_t* b, int n = 20) {
    Bn r;
    for (int i = 0; i < n; i++) {
        int bit = (n - 1 - i) * 8;
        r.w[bit / 32] |= (uint32_t)b[i] << (bit % 32);
    }
    return r;
}
void toBytes(const Bn& a, uint8_t* b, int n = 20) {
    for (int i = 0; i < n; i++) {
        int bit = (n - 1 - i) * 8;
        b[i] = (uint8_t)(a.w[bit / 32] >> (bit % 32));
    }
}
Bn fromHex(const char* s) {
    uint8_t b[20];
    for (int i = 0; i < 20; i++) {
        auto h = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        b[i] = (uint8_t)(h(s[2 * i]) << 4 | h(s[2 * i + 1]));
    }
    return fromBytes(b);
}
int cmp(const Bn& a, const Bn& b) {
    for (int i = L - 1; i >= 0; i--)
        if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    return 0;
}
bool isZero(const Bn& a) {
    for (int i = 0; i < L; i++)
        if (a.w[i]) return false;
    return true;
}
uint32_t addRaw(Bn& r, const Bn& a, const Bn& b) {
    uint64_t c = 0;
    for (int i = 0; i < L; i++) {
        c += (uint64_t)a.w[i] + b.w[i];
        r.w[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}
uint32_t subRaw(Bn& r, const Bn& a, const Bn& b) {
    int64_t c = 0;
    for (int i = 0; i < L; i++) {
        c += (int64_t)a.w[i] - b.w[i];
        r.w[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)(c & 1);
}
Bn addm(const Bn& a, const Bn& b, const Bn& m) {
    Bn r;
    addRaw(r, a, b);
    if (cmp(r, m) >= 0) subRaw(r, r, m);
    return r;
}
Bn subm(const Bn& a, const Bn& b, const Bn& m) {
    Bn r;
    if (subRaw(r, a, b)) addRaw(r, r, m);
    return r;
}
int bitLen(const Bn& a) {
    for (int i = L - 1; i >= 0; i--)
        if (a.w[i])
            for (int b = 31; b >= 0; b--)
                if (a.w[i] >> b & 1) return i * 32 + b + 1;
    return 0;
}
bool bit(const Bn& a, int i) { return (a.w[i / 32] >> (i % 32)) & 1; }
Bn mulm(const Bn& a, const Bn& b, const Bn& m) {
    Bn r;
    for (int i = bitLen(b) - 1; i >= 0; i--) {
        r = addm(r, r, m);
        if (bit(b, i)) r = addm(r, a, m);
    }
    return r;
}
Bn reduce(Bn a, const Bn& m) {  // a < 2^192, generic slow reduction
    Bn r;
    for (int i = bitLen(a) - 1; i >= 0; i--) {
        r = addm(r, r, m);
        if (bit(a, i)) {
            Bn one;
            one.w[0] = 1;
            r = addm(r, one, m);
        }
    }
    return r;
}
Bn powm(const Bn& a, const Bn& e, const Bn& m) {
    Bn r;
    r.w[0] = 1;
    for (int i = bitLen(e) - 1; i >= 0; i--) {
        r = mulm(r, r, m);
        if (bit(e, i)) r = mulm(r, a, m);
    }
    return r;
}
Bn invm(const Bn& a, const Bn& m) {  // m prime
    Bn two;
    two.w[0] = 2;
    Bn e;
    subRaw(e, m, two);
    return powm(a, e, m);
}

struct Curve {
    Bn p, a, b, n, gx, gy;
    Curve() {
        p = fromHex("FFFFFFFFFFFFFFFF00000001FFFFFFFFFFFFFFFF");
        a = fromHex("FFFFFFFFFFFFFFFF00000001FFFFFFFFFFFFFFFC");
        // KIRK command 0x10/0x11 curve (not the CMD1 curve).
        b = fromHex("A68BEDC33418029C1D3CE33B9A321FCCBB9E0F0B");
        n = fromHex("FFFFFFFFFFFFFFFEFFFFB5AE3C523E63944F2127");
        gx = fromHex("128EC4256487FD8FDF64E2437BC0A1F6D5AFDE2C");
        gy = fromHex("5958557EB1DB001260425524DBC379D5AC5F4ADF");
    }
};
const Curve& C() {
    static const Curve c;
    return c;
}

// Jacobian point; z == 0 means infinity.
struct Pt {
    Bn x, y, z;
};
Pt affine(const Bn& x, const Bn& y) {
    Pt r{x, y, {}};
    r.z.w[0] = 1;
    return r;
}
Pt dbl(const Pt& P) {
    const Bn& p = C().p;
    if (isZero(P.z) || isZero(P.y)) return Pt{};
    Bn yy = mulm(P.y, P.y, p);
    Bn s = mulm(P.x, yy, p);
    s = addm(s, s, p);
    s = addm(s, s, p);  // S = 4*X*Y^2
    Bn zz = mulm(P.z, P.z, p);
    Bn m = mulm(P.x, P.x, p);
    Bn m3 = addm(addm(m, m, p), m, p);
    Bn az4 = mulm(C().a, mulm(zz, zz, p), p);
    m = addm(m3, az4, p);  // M = 3X^2 + aZ^4
    Bn x3 = subm(mulm(m, m, p), addm(s, s, p), p);
    Bn yyyy = mulm(yy, yy, p);
    Bn y8 = addm(yyyy, yyyy, p);
    y8 = addm(y8, y8, p);
    y8 = addm(y8, y8, p);
    Bn y3 = subm(mulm(m, subm(s, x3, p), p), y8, p);
    Bn z3 = mulm(P.y, P.z, p);
    z3 = addm(z3, z3, p);
    return Pt{x3, y3, z3};
}
Pt add(const Pt& P, const Pt& Q) {
    const Bn& p = C().p;
    if (isZero(P.z)) return Q;
    if (isZero(Q.z)) return P;
    Bn z1z1 = mulm(P.z, P.z, p), z2z2 = mulm(Q.z, Q.z, p);
    Bn u1 = mulm(P.x, z2z2, p), u2 = mulm(Q.x, z1z1, p);
    Bn s1 = mulm(P.y, mulm(Q.z, z2z2, p), p), s2 = mulm(Q.y, mulm(P.z, z1z1, p), p);
    if (cmp(u1, u2) == 0) {
        if (cmp(s1, s2) == 0) return dbl(P);
        return Pt{};
    }
    Bn h = subm(u2, u1, p), r = subm(s2, s1, p);
    Bn hh = mulm(h, h, p), hhh = mulm(hh, h, p);
    Bn v = mulm(u1, hh, p);
    Bn x3 = subm(subm(mulm(r, r, p), hhh, p), addm(v, v, p), p);
    Bn y3 = subm(mulm(r, subm(v, x3, p), p), mulm(s1, hhh, p), p);
    Bn z3 = mulm(mulm(P.z, Q.z, p), h, p);
    return Pt{x3, y3, z3};
}
Pt mul(const Bn& k, const Pt& P) {
    Pt r{};
    for (int i = bitLen(k) - 1; i >= 0; i--) {
        r = dbl(r);
        if (bit(k, i)) r = add(r, P);
    }
    return r;
}
void toAffine(const Pt& P, Bn& x, Bn& y) {
    const Bn& p = C().p;
    Bn zi = invm(P.z, p);
    Bn zi2 = mulm(zi, zi, p);
    x = mulm(P.x, zi2, p);
    y = mulm(P.y, mulm(zi2, zi, p), p);
}

}  // namespace

void publicKey(const uint8_t priv[20], uint8_t pub[40]) {
    Bn d = fromBytes(priv), x, y;
    toAffine(mul(d, affine(C().gx, C().gy)), x, y);
    toBytes(x, pub);
    toBytes(y, pub + 20);
}

void sign(const uint8_t priv[20], const uint8_t hash[20], uint8_t sig[40], uint32_t nonceSeed) {
    const Curve& c = C();
    Bn d = reduce(fromBytes(priv), c.n);
    Bn e = reduce(fromBytes(hash), c.n);
    for (uint32_t ctr = 0;; ctr++) {
        uint8_t msg[20 + 20 + 8], kb[20];
        memcpy(msg, priv, 20);
        memcpy(msg + 20, hash, 20);
        for (int i = 0; i < 4; i++) msg[40 + i] = (uint8_t)(nonceSeed >> (8 * i));
        for (int i = 0; i < 4; i++) msg[44 + i] = (uint8_t)(ctr >> (8 * i));
        hmacSha1(priv, 20, msg, sizeof(msg), kb);
        Bn k = reduce(fromBytes(kb), c.n);
        if (isZero(k)) continue;
        Bn x, y;
        toAffine(mul(k, affine(c.gx, c.gy)), x, y);
        Bn r = reduce(x, c.n);
        if (isZero(r)) continue;
        Bn s = mulm(invm(k, c.n), addm(e, mulm(r, d, c.n), c.n), c.n);
        if (isZero(s)) continue;
        toBytes(r, sig);
        toBytes(s, sig + 20);
        return;
    }
}

bool verify(const uint8_t pub[40], const uint8_t hash[20], const uint8_t sig[40]) {
    const Curve& c = C();
    Bn r = fromBytes(sig), s = fromBytes(sig + 20);
    if (isZero(r) || isZero(s) || cmp(r, c.n) >= 0 || cmp(s, c.n) >= 0) return false;
    Bn e = reduce(fromBytes(hash), c.n);
    Bn w = invm(s, c.n);
    Bn u1 = mulm(e, w, c.n), u2 = mulm(r, w, c.n);
    Pt X = add(mul(u1, affine(c.gx, c.gy)), mul(u2, affine(fromBytes(pub), fromBytes(pub + 20))));
    if (isZero(X.z)) return false;
    Bn x, y;
    toAffine(X, x, y);
    return cmp(reduce(x, c.n), r) == 0;
}

}  // namespace ecdsa
}  // namespace ipbp
