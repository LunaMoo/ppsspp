// SPDX-License-Identifier: see LICENSE
#include "lzrc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace ipbp {
namespace psp {
namespace {

struct Model {
    uint8_t literal[8][256];
    uint8_t distBits[8][39];
    uint8_t dist[18][8];
    uint8_t match[8][8];
    uint8_t len[8][31];
    Model() { memset(this, 0x80, sizeof(*this)); }
};

// ------------------------------------------------------------------ decoder

struct Decoder {
    const uint8_t* in;
    int pos, len;
    uint32_t range = 0xFFFFFFFF, code = 0;
    bool err = false;

    uint8_t next() {
        if (pos >= len) {
            err = true;
            return 0;
        }
        return in[pos++];
    }
    void normalize() {
        if (range < 0x01000000) {
            range <<= 8;
            code = (code << 8) | next();
        }
    }
    int bit(uint8_t* p) {
        normalize();
        uint32_t bound = (range >> 8) * *p;
        *p -= *p >> 3;
        if (code < bound) {
            range = bound;
            *p += 31;
            return 1;
        }
        code -= bound;
        range -= bound;
        return 0;
    }
    int number(uint8_t* p, int n) {
        int num = 1;
        if (n > 3) {
            num = (num << 1) + bit(p + 3);
            if (n > 4) {
                num = (num << 1) + bit(p + 3);
                if (n > 5) {
                    normalize();
                    for (int i = 0; i < n - 5; i++) {
                        range >>= 1;
                        num <<= 1;
                        if (code < range) num += 1;
                        else code -= range;
                    }
                }
            }
        }
        if (n > 0) {
            num = (num << 1) + bit(p);
            if (n > 1) {
                num = (num << 1) + bit(p + 1);
                if (n > 2) num = (num << 1) + bit(p + 2);
            }
        }
        return num;
    }
};

// ------------------------------------------------------------------ encoder

struct Encoder {
    uint8_t* out;
    int pos = 0, cap;
    uint32_t range = 0xFFFFFFFF, low = 0;
    uint32_t cache = 0xFFFFFFFF;  // pending top byte (+carry), 0xFFFFFFFF = none
    bool overflow = false;
    // Optional: per probability slot counts of 0 and 1 bits (index = slot - statsBase).
    const uint8_t* statsBase = nullptr;
    uint32_t (*stats)[2] = nullptr;

    void put(uint8_t b) {
        if (pos >= cap) {
            overflow = true;
            return;
        }
        out[pos++] = b;
    }
    void normalize() {
        if (range < 0x01000000) {
            if (cache != 0xFFFFFFFF) {
                if (cache > 0xFF) {  // propagate carry into already written bytes
                    for (int p = pos - 1; p >= 0; p--)
                        if (++out[p] != 0) break;
                }
                put((uint8_t)cache);
            }
            cache = low >> 24;
            range <<= 8;
            low <<= 8;
        }
    }
    void addLow(uint32_t v) {
        uint32_t old = low;
        low += v;
        if (low < old) cache++;
    }
    void bit(uint8_t* p, int b) {
        if (stats) stats[p - statsBase][b != 0]++;
        normalize();
        uint32_t bound = (range >> 8) * *p;
        *p -= *p >> 3;
        if (b) {
            range = bound;
            *p += 31;
        } else {
            addLow(bound);
            range -= bound;
        }
    }
    void bittree(uint8_t* probs, int max, int number) {
        number += max;
        int n = 0;
        for (int t = number; t > 1; t >>= 1) n++;
        do {
            bit(probs + (number >> n), (number >> (n - 1)) & 1);
            n--;
        } while (n);
    }
    void numberBits(uint8_t* p, int n, int number) {
        if (n > 3) {
            bit(p + 3, (number >> (n - 1)) & 1);
            if (n > 4) {
                bit(p + 3, (number >> (n - 2)) & 1);
                if (n > 5) {
                    normalize();
                    for (int j = n - 3; j >= 3; j--) {
                        range >>= 1;
                        if (!((number >> j) & 1)) addLow(range);
                    }
                }
            }
        }
        int low3 = n < 3 ? n : 3;
        for (int k = 0; k < low3; k++) bit(p + k, (number >> (low3 - 1 - k)) & 1);
    }
    void flush() {
        range = 0;  // force a final normalize of the pending byte
        if (cache != 0xFFFFFFFF) {
            if (cache > 0xFF)
                for (int p = pos - 1; p >= 0; p--)
                    if (++out[p] != 0) break;
            put((uint8_t)cache);
        }
        put((uint8_t)(low >> 24));
        put((uint8_t)(low >> 16));
        put((uint8_t)(low >> 8));
        put((uint8_t)low);
    }
};

inline int log2i(int v) {
    int n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

constexpr int kLc = 5;
constexpr int kMaxMatch = 255;    // number = len-1 <= 254 (0xFF is the end marker)
constexpr int kMaxDist = 0xFEFF;  // keep inside the original 0xFF00 window

}  // namespace

int lzrcDecompress(uint8_t* out, int outCap, const uint8_t* in, int inLen) {
    if (inLen < 5) return -1;
    Decoder d{in, 0, inLen};
    uint8_t lc = d.next();
    d.code = (uint32_t)d.next() << 24;
    d.code |= (uint32_t)d.next() << 16;
    d.code |= (uint32_t)d.next() << 8;
    d.code |= d.next();
    if (lc & 0x80) {  // stored
        if (d.code > (uint32_t)outCap || d.code > (uint32_t)(inLen - 5)) return -1;
        memcpy(out, in + 5, d.code);
        return (int)d.code;
    }
    Model* m = new Model;
    std::unique_ptr<Model> guard(m);
    int state = 0, outPos = 0;
    uint8_t last = 0;
    for (;;) {
        if (d.err) return -1;
        if (!d.bit(&m->match[state][0])) {
            if (state > 0) state--;
            uint8_t* probs = m->literal[(last >> lc) & 7];
            int i = 1;
            while (i < 0x100) i = (i << 1) + d.bit(&probs[i]);
            if (outPos >= outCap) return -1;
            out[outPos++] = (uint8_t)i;
        } else {
            int n = 0;
            while (n < 7 && d.bit(&m->match[state][n + 1])) n++;
            int matchLen = 1;
            if (n > 0) {
                matchLen = d.number(&m->len[state][(n - 1) * 4 + ((outPos << (n - 1)) & 3)], n);
                if (matchLen == 0xFF) return d.err ? -1 : outPos;
            }
            int limit = matchLen > 2 ? 0x2C : 8;
            uint8_t* probs = &m->distBits[n][matchLen > 2 ? 7 : 0];
            int i = 1;
            do {
                i = (i << 1) + d.bit(&probs[i]);
            } while (i < limit);
            int db = i - limit;
            if (db >= 18) return -1;
            int dist = db > 0 ? d.number(m->dist[db], db) : 1;
            if (dist > outPos || dist <= 0) return -1;
            if (outPos + matchLen + 1 > outCap) return -1;
            for (int k = 0; k <= matchLen; k++, outPos++) out[outPos] = out[outPos - dist];
            state = ((outPos + 1) & 1) + 6;
        }
        last = out[outPos - 1];
    }
}

int lzrcCompress(uint8_t* out, int outCap, const uint8_t* in, int inLen) {
    if (outCap < 6) return -1;
    std::unique_ptr<Model> mguard(new Model);
    Model* m = mguard.get();
    Encoder e{out, 0, outCap};
    e.put((uint8_t)kLc);

    // Hash chains over 3-byte prefixes.
    constexpr int kHashBits = 15;
    std::vector<int> head(1 << kHashBits, -1), prev(inLen > 0 ? inLen : 1, -1);
    auto hash3 = [&](int p) {
        return ((in[p] << 10) ^ (in[p + 1] << 5) ^ in[p + 2]) & ((1 << kHashBits) - 1);
    };
    auto insert = [&](int p) {
        if (p + 2 < inLen) {
            int h = hash3(p);
            prev[p] = head[h];
            head[h] = p;
        }
    };
    auto findMatch = [&](int p, int& bestDist) {
        int best = 1;
        bestDist = 0;
        int maxLen = inLen - p < kMaxMatch ? inLen - p : kMaxMatch;
        if (maxLen < 2) return 1;
        // 2-byte matches only make sense at short distances (len<=3 requires dist<256)
        if (p + 2 < inLen) {
            int chain = 64;
            for (int c = head[hash3(p)]; c >= 0 && chain--; c = prev[c]) {
                int d = p - c;
                if (d > kMaxDist) break;
                if (in[c + best] != in[p + best]) continue;
                int l = 0;
                while (l < maxLen && in[c + l] == in[p + l]) l++;
                if (l <= 3 && d > 0xFF) continue;
                if (l > best) {
                    best = l;
                    bestDist = d;
                    if (l == maxLen) break;
                }
            }
        }
        if (best < 2) {
            for (int d = 1; d <= 0xFF && d <= p; d++) {
                if (in[p - d] == in[p] && in[p - d + 1] == in[p + 1]) {
                    best = 2;
                    bestDist = d;
                    if (maxLen > 2 && in[p - d + 2] == in[p + 2]) best = 3;
                    break;
                }
            }
        }
        return best;
    };

    int state = 0, pos = 0;
    uint8_t last = 0;
    auto emitLiteral = [&](int p) {
        e.bit(&m->match[state][0], 0);
        if (state > 0) state--;
        e.bittree(m->literal[(last >> kLc) & 7], 0x100, in[p]);
    };
    auto emitMatch = [&](int p, int len, int dist) {  // len = bytes to copy (2..255) or 0x100 for end
        int number = len - 1;
        e.bit(&m->match[state][0], 1);
        int n = log2i(number);
        for (int j = 0; j < n; j++) e.bit(&m->match[state][1 + j], 1);
        if (n < 7) e.bit(&m->match[state][1 + n], 0);
        if (n > 0) e.numberBits(&m->len[state][(n - 1) * 4 + ((p << (n - 1)) & 3)], n, number);
        if (number == 0xFF) return;
        int limit = number > 2 ? 0x2C : 8;
        int db = log2i(dist);
        e.bittree(&m->distBits[n][number > 2 ? 7 : 0], limit, db);
        if (db > 0) e.numberBits(m->dist[db], db, dist);
        state = ((p + len + 1) & 1) + 6;
    };

    while (pos < inLen) {
        int dist;
        int len = findMatch(pos, dist);
        if (len >= 2 && len < kMaxMatch && pos + 1 < inLen) {
            // one-step lazy evaluation
            insert(pos);
            int dist2;
            int len2 = findMatch(pos + 1, dist2);
            if (len2 > len + 1) {
                emitLiteral(pos);
                last = in[pos];
                pos++;
                len = len2;
                dist = dist2;
                for (int k = 1; k < len; k++) insert(pos + k);
                insert(pos);  // (pos itself inserted below via loop start? keep simple)
                emitMatch(pos, len, dist);
                pos += len;
                last = in[pos - 1];
                if (e.overflow) return -1;
                continue;
            }
            for (int k = 1; k < len; k++) insert(pos + k);
            emitMatch(pos, len, dist);
            pos += len;
            last = in[pos - 1];
        } else if (len >= 2) {
            for (int k = 0; k < len; k++) insert(pos + k);
            emitMatch(pos, len, dist);
            pos += len;
            last = in[pos - 1];
        } else {
            insert(pos);
            emitLiteral(pos);
            last = in[pos];
            pos++;
        }
        if (e.overflow) return -1;
    }
    emitMatch(pos, 0x100, 0);  // end marker
    e.flush();
    return e.overflow ? -1 : e.pos;
}

// ------------------------------------------------------------------ max: optimal parsing
//
// Same stream format and limits as lzrcCompress (so the PSP firmware's decoder reads it), but
// chooses literals and matches by their cost under the decoder's own adaptive model instead of
// greedily: a dynamic program over the block finds the cheapest sequence of tokens, with bit
// prices taken from the model a previous encoding of the block ended with. Two rounds let the
// prices settle. The smallest of these and the regular encoder's output is returned, so the
// result is never larger than lzrcCompress.

namespace {

struct Token {
    int len;   // 1 = literal, otherwise bytes copied (2..255)
    int dist;  // match distance, 0 for literals
};

constexpr int kPriceShift = 6;  // prices are in 1/64 bit
int kMaxRounds = 3;
struct PriceTable {
    uint32_t p1[257];  // cost of a 1 bit when the probability byte is p (P(1) = p/256)
    PriceTable() {
        for (int p = 1; p <= 256; p++) p1[p] = (uint32_t)(-std::log2(p / 256.0) * (1 << kPriceShift) + 0.5);
        p1[0] = p1[1] * 2;
    }
};
const PriceTable& prices() {
    static const PriceTable t;  // thread-safe initialisation
    return t;
}

constexpr int kSlots = (int)sizeof(Model);

// Bit costs per probability slot of the model, used to price tokens.
struct SlotPrices {
    uint32_t cost[kSlots][2];
    // From the bits each slot coded in the previous round (with a small prior).
    void fromStats(const uint32_t (*st)[2]) {
        for (int k = 0; k < kSlots; k++) {
            double n0 = st[k][0] + 0.4, n1 = st[k][1] + 0.4;
            cost[k][0] = (uint32_t)(-std::log2(n0 / (n0 + n1)) * (1 << kPriceShift) + 0.5);
            cost[k][1] = (uint32_t)(-std::log2(n1 / (n0 + n1)) * (1 << kPriceShift) + 0.5);
        }
    }
    // From a model snapshot.
    void fromModel(const Model& m) {
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(&m);
        const uint32_t* p1 = prices().p1;
        for (int k = 0; k < kSlots; k++) {
            cost[k][1] = p1[pb[k]];
            cost[k][0] = p1[256 - pb[k]];
        }
    }
};

struct Pricer {
    const Model& m;  // only for slot addresses
    const SlotPrices& sp;
    uint32_t bitAt(const uint8_t* slot, int b) const { return sp.cost[slot - reinterpret_cast<const uint8_t*>(&m)][b != 0]; }
    uint32_t bittree(const uint8_t* probs, int max, int number) const {
        number += max;
        int n = log2i(number);
        uint32_t c = 0;
        do {
            c += bitAt(&probs[number >> n], (number >> (n - 1)) & 1);
            n--;
        } while (n);
        return c;
    }
    uint32_t numberBits(const uint8_t* p, int n, int number) const {
        uint32_t c = 0;
        if (n > 3) {
            c += bitAt(&p[3], (number >> (n - 1)) & 1);
            if (n > 4) {
                c += bitAt(&p[3], (number >> (n - 2)) & 1);
                if (n > 5) c += (uint32_t)(n - 5) << kPriceShift;
            }
        }
        int low3 = n < 3 ? n : 3;
        for (int k = 0; k < low3; k++) c += bitAt(&p[k], (number >> (low3 - 1 - k)) & 1);
        return c;
    }
    uint32_t literal(int state, uint8_t last, uint8_t byte) const {
        return bitAt(&m.match[state][0], 0) + bittree(m.literal[(last >> kLc) & 7], 0x100, byte);
    }
    // Flag + length part of a match starting at pos.
    uint32_t matchHead(int state, int pos, int len) const {
        int number = len - 1;
        int n = log2i(number);
        uint32_t c = bitAt(&m.match[state][0], 1);
        for (int j = 0; j < n; j++) c += bitAt(&m.match[state][1 + j], 1);
        if (n < 7) c += bitAt(&m.match[state][1 + n], 0);
        if (n > 0) c += numberBits(&m.len[state][(n - 1) * 4 + ((pos << (n - 1)) & 3)], n, number);
        return c;
    }
    uint32_t matchDist(int len, int dist) const {
        int number = len - 1;
        int n = log2i(number);
        int db = log2i(dist);
        uint32_t c = bittree(&m.distBits[n][number > 2 ? 7 : 0], number > 2 ? 0x2C : 8, db);
        if (db > 0) c += numberBits(m.dist[db], db, dist);
        return c;
    }
};

// Writes tokens exactly like lzrcCompress's emit code. finalModel (optional) receives the model
// state at the end, which prices the next round.
int encodeTokens(uint8_t* out, int outCap, const uint8_t* in, const std::vector<Token>& tokens, Model* finalModel,
                 uint32_t (*stats)[2] = nullptr) {
    if (outCap < 6) return -1;
    std::unique_ptr<Model> mguard(new Model);
    Model* m = mguard.get();
    Encoder e{out, 0, outCap};
    if (stats) {
        memset(stats, 0, sizeof(uint32_t) * 2 * kSlots);
        e.statsBase = reinterpret_cast<const uint8_t*>(m);
        e.stats = stats;
    }
    e.put((uint8_t)kLc);
    int state = 0, pos = 0;
    uint8_t last = 0;
    auto emitMatch = [&](int p, int len, int dist) {
        int number = len - 1;
        e.bit(&m->match[state][0], 1);
        int n = log2i(number);
        for (int j = 0; j < n; j++) e.bit(&m->match[state][1 + j], 1);
        if (n < 7) e.bit(&m->match[state][1 + n], 0);
        if (n > 0) e.numberBits(&m->len[state][(n - 1) * 4 + ((p << (n - 1)) & 3)], n, number);
        if (number == 0xFF) return;
        int limit = number > 2 ? 0x2C : 8;
        int db = log2i(dist);
        e.bittree(&m->distBits[n][number > 2 ? 7 : 0], limit, db);
        if (db > 0) e.numberBits(m->dist[db], db, dist);
        state = ((p + len + 1) & 1) + 6;
    };
    for (const Token& t : tokens) {
        if (t.len == 1) {
            e.bit(&m->match[state][0], 0);
            if (state > 0) state--;
            e.bittree(m->literal[(last >> kLc) & 7], 0x100, in[pos]);
            pos++;
        } else {
            emitMatch(pos, t.len, t.dist);
            pos += t.len;
        }
        last = in[pos - 1];
        if (e.overflow) return -1;
    }
    emitMatch(pos, 0x100, 0);  // end marker
    e.flush();
    if (finalModel) *finalModel = *m;
    return e.overflow ? -1 : e.pos;
}

struct Candidate {
    uint16_t len, dist;
};

// For each position, matches of increasing length with the nearest distance found for each,
// plus the nearest short-range match (distance < 256, the limit for 2- and 3-byte matches).
void findCandidates(const uint8_t* in, int inLen, std::vector<uint32_t>& start, std::vector<Candidate>& cands) {
    constexpr int kHashBits = 15;
    constexpr int kChain = 512;
    std::vector<int> head(1 << kHashBits, -1), prev(inLen > 0 ? inLen : 1, -1);
    auto hash3 = [&](int p) { return ((in[p] << 10) ^ (in[p + 1] << 5) ^ in[p + 2]) & ((1 << kHashBits) - 1); };
    start.assign(inLen + 1, 0);
    cands.clear();
    for (int p = 0; p < inLen; p++) {
        start[p] = (uint32_t)cands.size();
        int maxLen = inLen - p < kMaxMatch ? inLen - p : kMaxMatch;
        if (maxLen >= 2) {
            // Nearest 2+ byte match within 255 bytes (the only distances 2-3 byte matches may use).
            for (int d = 1; d <= 0xFF && d <= p; d++) {
                if (in[p - d] == in[p] && in[p - d + 1] == in[p + 1]) {
                    int l = 2;
                    while (l < maxLen && in[p - d + l] == in[p + l]) l++;
                    cands.push_back({(uint16_t)l, (uint16_t)d});
                    break;
                }
            }
            int best = cands.size() > start[p] ? cands.back().len : 1;
            if (p + 2 < inLen && best < maxLen) {
                int chain = kChain;
                for (int c = head[hash3(p)]; c >= 0 && chain--; c = prev[c]) {
                    int d = p - c;
                    if (d > kMaxDist) break;
                    if (in[c + best] != in[p + best]) continue;
                    int l = 0;
                    while (l < maxLen && in[c + l] == in[p + l]) l++;
                    if (l > best && l >= 4) {
                        best = l;
                        cands.push_back({(uint16_t)l, (uint16_t)d});
                        if (l == maxLen) break;
                    }
                }
            }
        }
        if (p + 2 < inLen) {
            int h = hash3(p);
            prev[p] = head[h];
            head[h] = p;
        }
    }
    start[inLen] = (uint32_t)cands.size();
}

std::vector<Token> greedyParse(int inLen, const std::vector<uint32_t>& start, const std::vector<Candidate>& cands) {
    std::vector<Token> t;
    for (int p = 0; p < inLen;) {
        if (start[p + 1] > start[p]) {
            const Candidate& c = cands[start[p + 1] - 1];
            t.push_back({c.len, c.dist});
            p += c.len;
        } else {
            t.push_back({1, 0});
            p++;
        }
    }
    return t;
}

std::vector<Token> optimalParse(const uint8_t* in, int inLen, const std::vector<uint32_t>& start,
                                const std::vector<Candidate>& cands, const SlotPrices& sp) {
    constexpr int kNiceLen = 128;  // matches this long are taken whole, not split
    static const Model layout;
    Pricer pr{layout, sp};
    const uint32_t kInf = 0xFFFFFFFF;
    std::vector<uint32_t> cost(inLen + 1, kInf);
    std::vector<int> fromLen(inLen + 1, 0), fromDist(inLen + 1, 0);
    std::vector<uint8_t> stateAt(inLen + 1, 0);
    cost[0] = 0;
    for (int p = 0; p < inLen; p++) {
        if (cost[p] == kInf) continue;
        const int st = stateAt[p];
        const uint8_t last = p > 0 ? in[p - 1] : 0;
        // literal
        uint32_t c = cost[p] + pr.literal(st, last, in[p]);
        if (c < cost[p + 1]) {
            cost[p + 1] = c;
            fromLen[p + 1] = 1;
            fromDist[p + 1] = 0;
            stateAt[p + 1] = (uint8_t)(st > 0 ? st - 1 : 0);
        }
        // matches
        int covered = 1;  // lengths up to this were offered by a nearer candidate
        for (uint32_t k = start[p]; k < start[p + 1]; k++) {
            const Candidate& cd = cands[k];
            int from = covered + 1, to = cd.len;
            if (cd.len >= kNiceLen) from = cd.len;
            uint32_t distPrice = 0;
            int distN = -1;
            bool distShort = false;
            for (int l = from; l <= to; l++) {
                if (l <= 3 && cd.dist > 0xFF) continue;
                int number = l - 1;
                int n = log2i(number);
                bool shortMatch = number <= 2;
                if (n != distN || shortMatch != distShort) {
                    distPrice = pr.matchDist(l, cd.dist);
                    distN = n;
                    distShort = shortMatch;
                }
                uint32_t mc = cost[p] + pr.matchHead(st, p, l) + distPrice;
                int q = p + l;
                if (mc < cost[q]) {
                    cost[q] = mc;
                    fromLen[q] = l;
                    fromDist[q] = cd.dist;
                    stateAt[q] = (uint8_t)(((q + 1) & 1) + 6);
                }
            }
            if (cd.len > covered) covered = cd.len;
        }
    }
    std::vector<Token> t;
    for (int q = inLen; q > 0; q -= fromLen[q]) t.push_back({fromLen[q], fromDist[q]});
    std::reverse(t.begin(), t.end());
    return t;
}

}  // namespace

int lzrcCompressMax(uint8_t* out, int outCap, const uint8_t* in, int inLen) {
    // The regular encoder is the baseline: max never returns anything larger.
    int bestSize = lzrcCompress(out, outCap, in, inLen);
    if (inLen <= 0) return bestSize;
    // Already-compressed data (video, audio) that the regular encoder leaves above 95% gains too
    // little to matter (and won't reach the 90% at which NPUMDIMG blocks are stored compressed).
    if (bestSize < 0 || (int64_t)bestSize * 100 > (int64_t)inLen * 95) return bestSize;

    std::vector<uint32_t> start;
    std::vector<Candidate> cands;
    findCandidates(in, inLen, start, cands);

    std::vector<uint8_t> tmp(outCap > 0 ? outCap : 1);
    std::unique_ptr<uint32_t[][2]> stats(new uint32_t[kSlots][2]);
    std::unique_ptr<SlotPrices> sp(new SlotPrices);
    auto keep = [&](int size) {
        if (size > 0 && (bestSize < 0 || size < bestSize)) {
            memcpy(out, tmp.data(), size);
            bestSize = size;
        }
    };
    // Round 0 measures a greedy parse; each later round parses optimally with the bit costs the
    // previous round's tokens had, which converges in a few rounds.
    keep(encodeTokens(tmp.data(), outCap, in, greedyParse(inLen, start, cands), nullptr, stats.get()));
    for (int round = 0; round < kMaxRounds; round++) {
        sp->fromStats(stats.get());
        keep(encodeTokens(tmp.data(), outCap, in, optimalParse(in, inLen, start, cands, *sp), nullptr, stats.get()));
    }
    return bestSize;
}

}  // namespace psp
}  // namespace ipbp
