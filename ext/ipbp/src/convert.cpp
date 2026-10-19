// SPDX-License-Identifier: see LICENSE
//
// ISO <-> NPUMDIMG EBOOT.PBP conversion. Behaviour mirrors sign_np 1.0.4 (Hykem, mod by LMAN)
// for ISO -> PBP and npdpc_mod (tpu, mod by LMAN) for PBP -> ISO, which were the backends of
// the IsoPbpConverter GUI.
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "crypto/ecdsa.h"
#include "crypto/sha1.h"
#include "../include/ipbp/ipbp.hpp"
#include "psp/amctrl.h"
#include "psp/iso9660.h"
#include "psp/lzrc.h"
#include "psp/sfo.h"
#include "util.h"

namespace ipbp {

using detail::rd32;
using detail::wr16;
using detail::wr32;

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Cancelled: return "cancelled";
        case Status::IoError: return "I/O error";
        case Status::InvalidInput: return "invalid input";
        case Status::Unsupported: return "unsupported";
        case Status::VerifyFailed: return "verification failed";
    }
    return "unknown";
}

namespace {

constexpr uint32_t kBlockBasis = 0x10;             // sectors per block (sign_np)
constexpr uint32_t kBlockSize = kBlockBasis * 0x800;  // 32 KiB
constexpr uint32_t kTableEntry = 0x20;

// ECDSA key pair used by sign_np for DATA.PSP and the NPUMDIMG header (KIRK CMD 0x10/0x11 curve).
const uint8_t kSignPriv[20] = {0x14, 0xB0, 0x22, 0xE8, 0x92, 0xCF, 0x86, 0x14, 0xA4, 0x45,
                               0x57, 0xDB, 0x09, 0x5C, 0x92, 0x8D, 0xE9, 0xB8, 0x99, 0x70};

unsigned resolveThreads(unsigned t) {
    if (t) return t;
    unsigned h = std::thread::hardware_concurrency();
    return h ? (h > 16 ? 16 : h) : 2;
}

#if defined(__GNUC__)
void logf(const Callbacks& cb, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
void logf(const Callbacks& cb, const char* fmt, ...) {
    if (!cb.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cb.log(buf);
}

// Random bytes: OS randomness, or a SHA-1 based stream for reproducible output.
class RandomSource {
public:
    explicit RandomSource(const std::optional<uint64_t>& seed) : det_(seed.has_value()), seed_(seed.value_or(0)) {}
    void fill(uint8_t* out, size_t n, const char* label) {
        if (det_) {
            for (size_t off = 0; off < n; off += 20) {
                uint8_t in[64] = {0}, h[20];
                for (int i = 0; i < 8; i++) in[i] = (uint8_t)(seed_ >> (8 * i));
                strncpy(reinterpret_cast<char*>(in + 8), label, 40);
                wr32(in + 48, (uint32_t)off);
                Sha1::hash(in, sizeof(in), h);
                memcpy(out + off, h, std::min<size_t>(20, n - off));
            }
        } else {
            std::random_device rd;
            for (size_t i = 0; i < n; i += 4) {
                uint32_t v = rd();
                for (size_t k = 0; k < 4 && i + k < n; k++) out[i + k] = (uint8_t)(v >> (8 * k));
            }
        }
    }
    uint32_t nonceSeed(uint32_t salt) const { return det_ ? (uint32_t)(seed_ * 2654435761u) ^ salt : 0; }

private:
    bool det_;
    uint64_t seed_;
};

void scrambleEntry(uint8_t* e) {  // involution: same for scramble and unscramble
    uint32_t p[8];
    for (int i = 0; i < 8; i++) p[i] = rd32(e + 4 * i);
    p[4] ^= p[3] ^ p[2];
    p[5] ^= p[2] ^ p[1];
    p[6] ^= p[3] ^ p[0];
    p[7] ^= p[0] ^ p[1];
    for (int i = 4; i < 8; i++) wr32(e + 4 * i, p[i]);
}

bool isValidContentId(const std::string& cid) {
    // XXYYYY-AAAABBBBB_CC-DDDDDDDDDDDDDDDD
    if (cid.size() != 36) return false;
    return cid[6] == '-' && cid[16] == '_' && cid[19] == '-';
}

}  // namespace

// ============================================================================ ISO -> PBP

Result isoToPbp(InputStream& iso, OutputStream& out, const IsoToPbpOptions& opt, const Callbacks& cb,
                uint64_t* outPbpSize) {
    psp::IsoReader reader(iso);
    std::string err;
    if (!reader.open(&err)) return Result::fail(Status::InvalidInput, err);

    std::vector<uint8_t> sfo, icon0, icon1, pic0, pic1, snd0;
    if (!reader.readFile("/PSP_GAME/PARAM.SFO", sfo))
        return Result::fail(Status::InvalidInput, "not a PSP game image: /PSP_GAME/PARAM.SFO not found");
    reader.readFile("/PSP_GAME/ICON0.PNG", icon0);
    reader.readFile("/PSP_GAME/ICON1.PMF", icon1);
    reader.readFile("/PSP_GAME/PIC0.PNG", pic0);
    reader.readFile("/PSP_GAME/PIC1.PNG", pic1);
    reader.readFile("/PSP_GAME/SND0.AT3", snd0);

    psp::Sfo param(sfo);
    if (!param.valid()) return Result::fail(Status::InvalidInput, "PARAM.SFO is corrupt");
    std::string discId, title, sysVer;
    param.getString("DISC_ID", discId);
    param.getString("TITLE", title);
    param.getString("PSP_SYSTEM_VER", sysVer);
    if (discId.empty()) {
        std::vector<uint8_t> umd;
        if (reader.readFile("/UMD_DATA.BIN", umd) && umd.size() >= 10) {
            for (int i = 0; i < 10; i++)
                if (umd[i] != '-') discId += (char)umd[i];
        }
    }

    // Content id & version key.
    std::string cid = opt.contentId;
    if (cid.empty()) {
        if (discId.size() != 9)
            return Result::fail(Status::InvalidInput,
                                "cannot build a content id: DISC_ID '" + discId + "' is not 9 characters; pass one explicitly");
        cid = "UP0000-" + discId + "_00-0000000000000000";
    }
    if (!isValidContentId(cid))
        return Result::fail(Status::InvalidInput,
                            "content id must look like XXYYYY-AAAABBBBB_CC-DDDDDDDDDDDDDDDD (got '" + cid + "')");

    uint8_t vkey[16];
    uint32_t npFlags;
    if (opt.versionKey) {
        memcpy(vkey, opt.versionKey->data(), 16);
        npFlags = 0x00000002;
    } else {
        npFlags = 0x01000003;
        psp::npDrmGetFixedKey(cid.c_str(), npFlags, vkey);
    }

    logf(cb, "Title: %s", title.c_str());
    logf(cb, "DISC_ID: %s  PSP_SYSTEM_VER: %s", discId.c_str(), sysVer.c_str());
    logf(cb, "Content ID: %s", cid.c_str());
    logf(cb, "Version key: %s (%s)", detail::hex(vkey, 16).c_str(), opt.versionKey ? "user supplied" : "fixed key");

    // PARAM.SFO patching (as sign_np): DISC_ID from the content id, CATEGORY "EG".
    param.setStringInPlace("DISC_ID", cid.substr(7, 9));
    param.setStringInPlace("CATEGORY", "EG");

    RandomSource rnd(opt.deterministicSeed);

    // ---------------------------------------------------------------- DATA.PSP
    std::vector<uint8_t> startdat;
    if (!opt.startdatPng.empty()) {
        startdat.assign(0x50, 0);
        memcpy(&startdat[0], "STARTDAT", 8);
        wr32(&startdat[8], 1);
        wr32(&startdat[12], 1);
        wr32(&startdat[16], 0x50);
        wr32(&startdat[20], (uint32_t)opt.startdatPng.size());
        startdat.insert(startdat.end(), opt.startdatPng.begin(), opt.startdatPng.end());
    }
    size_t pgdOff = startdat.empty() ? 0x594 : startdat.size() + 0x5A0;
    std::vector<uint8_t> dataPsp(pgdOff + opt.pgd.size(), 0);
    memcpy(&dataPsp[0x560], cid.data(), cid.size());
    // NP flags, big-endian (sign_np byte-swaps them)
    dataPsp[0x590] = (uint8_t)(npFlags >> 24);
    dataPsp[0x591] = (uint8_t)(npFlags >> 16);
    dataPsp[0x592] = (uint8_t)(npFlags >> 8);
    dataPsp[0x593] = (uint8_t)npFlags;
    {
        Sha1 h;
        h.update(sfo.data(), sfo.size());
        h.update(&dataPsp[0x560], 0x30);
        uint8_t digest[20];
        h.finish(digest);
        ecdsa::sign(kSignPriv, digest, &dataPsp[0], rnd.nonceSeed(1));
    }
    if (!startdat.empty()) memcpy(&dataPsp[0x5A0], startdat.data(), startdat.size());
    if (!opt.pgd.empty()) {
        memcpy(&dataPsp[pgdOff], opt.pgd.data(), opt.pgd.size());
        wr32(&dataPsp[0x30], (uint32_t)pgdOff);
        wr32(&dataPsp[0x34], (uint32_t)opt.pgd.size());
    }

    // ---------------------------------------------------------------- PBP header + files
    std::vector<uint8_t> pbpHead(0x28, 0);
    memcpy(&pbpHead[0], "\0PBP", 4);
    wr32(&pbpHead[4], 0x00010001);
    const std::vector<uint8_t>* files[7] = {&sfo, &icon0, &icon1, &pic0, &pic1, &snd0, &dataPsp};
    uint64_t off = 0x28;
    for (int i = 0; i < 7; i++) {
        wr32(&pbpHead[8 + 4 * i], (uint32_t)off);
        off += files[i]->size();
    }
    uint64_t psar = (off + 15) & ~15ull;
    while (psar & 0xFF) psar += 0x10;
    wr32(&pbpHead[0x24], (uint32_t)psar);

    if (!out.writeAt(0, pbpHead.data(), pbpHead.size())) return Result::fail(Status::IoError, "write failed");
    off = 0x28;
    for (auto* f : files) {
        if (!f->empty() && !out.writeAt(off, f->data(), f->size())) return Result::fail(Status::IoError, "write failed");
        off += f->size();
    }
    if (psar > off) {
        std::vector<uint8_t> z(psar - off, 0);
        if (!out.writeAt(off, z.data(), z.size())) return Result::fail(Status::IoError, "write failed");
    }

    // ---------------------------------------------------------------- DATA.PSAR blocks
    const uint64_t isoSize = iso.size();
    const uint64_t blocks64 = (isoSize + kBlockSize - 1) / kBlockSize;
    if (blocks64 == 0 || blocks64 > 0x7FFFFFFF / kTableEntry)
        return Result::fail(Status::InvalidInput, "ISO size out of range");
    const uint32_t blocks = (uint32_t)blocks64;
    const uint32_t tableSize = blocks * kTableEntry;
    std::vector<uint8_t> table(tableSize, 0);

    uint8_t headerKey[16];
    rnd.fill(headerKey, 16, "npumdimg header key");

    {
        IsoDumpCheck dc;
        if (checkIsoDump(iso, dc) && !dc.good())
            logf(cb, "warning: ISO is %lld bytes %s its volume descriptor size (%s)", (long long)std::llabs(dc.difference()),
                 dc.difference() > 0 ? "larger than" : "smaller than", dc.difference() > 0 ? "overdump" : "underdump - bad dump?");
    }
    logf(cb, "ISO size: %llu bytes, %u blocks of 0x%X", (unsigned long long)isoSize, blocks, kBlockSize);

    const unsigned threads = resolveThreads(opt.threads);
    detail::WorkerPool pool(threads);
    const size_t batch = std::max<size_t>(4, threads * 4);
    struct Slot {
        std::vector<uint8_t> raw, packed;
        uint32_t size = 0;
        bool compressed = false;
        uint64_t offset = 0;  // relative to PSAR
    };
    std::vector<Slot> slots(batch);
    for (auto& s : slots) {
        s.raw.resize(kBlockSize);
        s.packed.resize(kBlockSize * 2 + 64);
    }

    uint64_t dataOff = 0x100 + tableSize;  // PSAR-relative position of next block
    uint32_t compressedBlocks = 0;
    if (cb.progress) cb.progress(0, isoSize);
    for (uint32_t first = 0; first < blocks; first += (uint32_t)batch) {
        if (cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        size_t n = std::min<size_t>(batch, blocks - first);
        // read
        for (size_t k = 0; k < n; k++) {
            uint64_t pos = (uint64_t)(first + k) * kBlockSize;
            size_t len = (size_t)std::min<uint64_t>(kBlockSize, isoSize - pos);
            Slot& s = slots[k];
            if (len < kBlockSize) memset(s.raw.data() + len, 0, kBlockSize - len);
            if (!iso.readAt(pos, s.raw.data(), len)) return Result::fail(Status::IoError, "read error on ISO");
        }
        // compress (parallel)
        std::atomic<bool> stop{false};
        pool.run(n, [&](size_t k) {
            if (stop || cb.cancel.cancelled()) {
                stop = true;
                return;
            }
            Slot& s = slots[k];
            s.compressed = false;
            s.size = kBlockSize;
            if (opt.compress) {
                int c = opt.maxCompression
                            ? psp::lzrcCompressMax(s.packed.data(), (int)s.packed.size() - 16, s.raw.data(), kBlockSize)
                            : psp::lzrcCompress(s.packed.data(), (int)s.packed.size() - 16, s.raw.data(), kBlockSize);
                if (c > 0 && (uint64_t)c * 100 <= 0x2CFFFF) {  // sign_np: keep only if <= ~90%
                    uint32_t aligned = (c + 15) & ~15u;
                    memset(s.packed.data() + c, 0, aligned - c);
                    s.size = aligned;
                    s.compressed = true;
                }
            }
        });
        if (stop || cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        // assign offsets
        for (size_t k = 0; k < n; k++) {
            slots[k].offset = dataOff;
            dataOff += slots[k].size;
            if (dataOff > 0xFFFFFFFFull) return Result::fail(Status::Unsupported, "PBP would exceed 4 GiB");
            compressedBlocks += slots[k].compressed;
        }
        // encrypt + MAC (parallel)
        pool.run(n, [&](size_t k) {
            Slot& s = slots[k];
            uint8_t* d = s.compressed ? s.packed.data() : s.raw.data();
            psp::BBCipher c(headerKey, vkey, (uint32_t)(s.offset >> 4));
            c.update(d, s.size);
            uint8_t* e = &table[(size_t)(first + k) * kTableEntry];
            psp::BBMac mac(3);
            mac.update(d, s.size);
            mac.final(e, vkey);
            psp::bbmacBuildFinal2(3, e);
            wr32(e + 0x10, (uint32_t)s.offset);
            wr32(e + 0x14, s.size);
            wr32(e + 0x18, 0);
            wr32(e + 0x1C, 0);
            scrambleEntry(e);
        });
        // write (sequential)
        for (size_t k = 0; k < n; k++) {
            Slot& s = slots[k];
            const uint8_t* d = s.compressed ? s.packed.data() : s.raw.data();
            if (!out.writeAt(psar + s.offset, d, s.size)) return Result::fail(Status::IoError, "write failed");
        }
        if (cb.progress) cb.progress(std::min<uint64_t>((uint64_t)(first + n) * kBlockSize, isoSize), isoSize);
    }

    // ---------------------------------------------------------------- table MAC + NPUMDIMG header
    uint8_t tableMac[16];
    {
        psp::BBMac mac(3);
        mac.update(table.data(), table.size());
        mac.final(tableMac, vkey);
        psp::bbmacBuildFinal2(3, tableMac);
    }

    uint8_t h[0x100] = {0};
    memcpy(h, "NPUMDIMG", 8);
    wr32(h + 0x08, npFlags);
    wr32(h + 0x0C, kBlockBasis);
    memcpy(h + 0x10, cid.data(), std::min<size_t>(cid.size(), 0x30));
    wr16(h + 0x40, 0x800);
    wr16(h + 0x42, isoSize > 0x40000000ull ? 0xE001 : 0xE000);
    wr32(h + 0x48, 0x1010);
    const uint32_t isoSectors = (uint32_t)((isoSize + 0x7FF) / 0x800);
    const uint32_t lbaEnd = isoSectors - 1;
    wr32(h + 0x5C, std::min<uint32_t>(lbaEnd, 0x6C0BF));
    wr32(h + 0x64, lbaEnd);
    wr32(h + 0x68, 0x01003FFE);
    wr32(h + 0x6C, 0x100);
    memcpy(h + 0x70, cid.data() + 7, 4);
    h[0x74] = '-';
    memcpy(h + 0x75, cid.data() + 11, 5);
    memcpy(h + 0xA0, headerKey, 16);
    memcpy(h + 0xB0, tableMac, 16);
    rnd.fill(h + 0xD0, 8, "npumdimg header random");
    {
        psp::BBCipher c(headerKey, vkey, 0);
        c.update(h + 0x40, 0x60);
        psp::BBMac mac(3);
        mac.update(h, 0xC0);
        mac.final(h + 0xC0, vkey);
        psp::bbmacBuildFinal2(3, h + 0xC0);
        uint8_t digest[20];
        Sha1::hash(h, 0xD8, digest);
        ecdsa::sign(kSignPriv, digest, h + 0xD8, rnd.nonceSeed(2));
    }
    if (!out.writeAt(psar, h, sizeof(h)) || !out.writeAt(psar + 0x100, table.data(), table.size()))
        return Result::fail(Status::IoError, "write failed");

    uint64_t total = psar + dataOff;
    if (!out.finish(total)) return Result::fail(Status::IoError, "failed to finalize output");
    if (outPbpSize) *outPbpSize = total;
    logf(cb, "Compressed blocks: %u/%u, PBP size: %llu bytes", compressedBlocks, blocks, (unsigned long long)total);
    return Result::success();
}

// ============================================================================ PBP parsing

namespace {

struct NpImage {
    uint64_t psar = 0;
    uint8_t header[0x100];  // decrypted
    uint8_t vkey[16];
    uint32_t basis = 0, blockSize = 0, blocks = 0;
    uint64_t lbaSize = 0;
    std::vector<uint8_t> table;  // unscrambled
    bool tableMacOk = false;
    std::vector<uint8_t> sfo;
};

Result parseNp(InputStream& in, NpImage& np, bool readTable) {
    uint8_t ph[0x28];
    if (in.size() < 0x28 || !in.readAt(0, ph, sizeof(ph)))
        return Result::fail(Status::InvalidInput, "file too small to be a PBP");
    if (memcmp(ph, "\0PBP", 4) != 0) return Result::fail(Status::InvalidInput, "not a PBP file");
    uint32_t sfoOff = rd32(ph + 8), sfoEnd = rd32(ph + 12);
    if (sfoEnd > sfoOff && sfoEnd - sfoOff < 0x10000 && sfoEnd <= in.size()) {
        np.sfo.resize(sfoEnd - sfoOff);
        if (!in.readAt(sfoOff, np.sfo.data(), np.sfo.size())) np.sfo.clear();
    }
    np.psar = rd32(ph + 0x24);
    uint8_t* h = np.header;
    if (np.psar + 0x100 > in.size() || !in.readAt(np.psar, h, 0x100))
        return Result::fail(Status::InvalidInput, "PBP has no DATA.PSAR");
    if (memcmp(h, "NPUMDIMG", 8) != 0) {
        if (memcmp(h, "PSISOIMG", 8) == 0 || memcmp(h, "PSTITLEI", 8) == 0)
            return Result::fail(Status::Unsupported, "this is a PS1 (PSISOIMG) EBOOT, not a PSP UMD image");
        return Result::fail(Status::InvalidInput, "DATA.PSAR is not an NPUMDIMG image");
    }
    {
        psp::BBMac mac(3);
        mac.update(h, 0xC0);
        psp::bbmacGetKey(mac, h + 0xC0, np.vkey);
    }
    {
        psp::BBCipher c(h + 0xA0, np.vkey, 0);
        c.update(h + 0x40, 0x60);
    }
    np.basis = rd32(h + 0x0C);
    if (np.basis == 0 || np.basis > 0x100) return Result::fail(Status::InvalidInput, "bad NPUMDIMG block size");
    np.blockSize = np.basis * 0x800;
    uint32_t lbaStart = rd32(h + 0x54), lbaEnd = rd32(h + 0x64);
    if (lbaEnd < lbaStart) return Result::fail(Status::InvalidInput, "bad NPUMDIMG LBA range");
    np.lbaSize = (uint64_t)lbaEnd - lbaStart + 1;
    np.blocks = (uint32_t)((np.lbaSize + np.basis - 1) / np.basis);
    if (!readTable) return Result::success();
    uint64_t tableOff = np.psar + rd32(h + 0x6C);
    uint64_t tableSize = (uint64_t)np.blocks * kTableEntry;
    if (tableOff + tableSize > in.size()) return Result::fail(Status::InvalidInput, "NPUMDIMG table is truncated");
    np.table.resize((size_t)tableSize);
    if (!in.readAt(tableOff, np.table.data(), np.table.size())) return Result::fail(Status::IoError, "read error");
    {
        psp::BBMac mac(3);
        mac.update(np.table.data(), np.table.size());
        np.tableMacOk = psp::bbmacVerify(mac, h + 0xB0, np.vkey);
    }
    for (uint32_t i = 0; i < np.blocks; i++) scrambleEntry(&np.table[(size_t)i * kTableEntry]);
    return Result::success();
}

}  // namespace

// ============================================================================ PBP -> ISO

Result pbpToIso(InputStream& in, OutputStream& out, const PbpToIsoOptions& opt, const Callbacks& cb,
                uint64_t* outIsoSize) {
    NpImage np;
    Result r = parseNp(in, np, true);
    if (!r) return r;
    const uint8_t* h = np.header;
    char cid[0x31] = {0};
    memcpy(cid, h + 0x10, 0x30);
    logf(cb, "Content ID: %s", cid);
    logf(cb, "Version key: %s", detail::hex(np.vkey, 16).c_str());
    logf(cb, "Blocks: %u x 0x%X, table size: %u", np.blocks, np.blockSize, np.blocks * kTableEntry);
    if (!np.tableMacOk) logf(cb, "warning: block table MAC does not match (continuing)");

    uint64_t isoSize = std::min<uint64_t>((uint64_t)np.blocks * np.blockSize, np.lbaSize * 0x800);
    const unsigned threads = resolveThreads(opt.threads);
    detail::WorkerPool pool(threads);
    const size_t batch = std::max<size_t>(4, threads * 4);
    struct Slot {
        std::vector<uint8_t> in, out;
        uint32_t size = 0;
        Result res;
    };
    std::vector<Slot> slots(batch);
    for (auto& s : slots) {
        s.in.resize(np.blockSize + 16);
        s.out.resize(np.blockSize);
    }
    uint64_t pvdVolume = 0;
    if (cb.progress) cb.progress(0, isoSize);
    for (uint32_t first = 0; first < np.blocks; first += (uint32_t)batch) {
        if (cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        size_t n = std::min<size_t>(batch, np.blocks - first);
        for (size_t k = 0; k < n; k++) {
            const uint8_t* e = &np.table[(size_t)(first + k) * kTableEntry];
            Slot& s = slots[k];
            s.res = Result::success();
            s.size = rd32(e + 0x14);
            if (rd32(e + 0x1C) != 0 || s.size == 0 || s.size > np.blockSize) {
                s.size = 0;  // unreadable entry
                continue;
            }
            uint64_t pos = np.psar + rd32(e + 0x10);
            if (pos + s.size > in.size() || !in.readAt(pos, s.in.data(), s.size))
                return Result::fail(Status::InvalidInput, "PBP is truncated (block " + std::to_string(first + k) + ")");
        }
        std::atomic<bool> stop{false};
        pool.run(n, [&](size_t k) {
            if (stop || cb.cancel.cancelled()) {
                stop = true;
                return;
            }
            uint32_t idx = first + (uint32_t)k;
            const uint8_t* e = &np.table[(size_t)idx * kTableEntry];
            Slot& s = slots[k];
            if (s.size == 0) {
                if (idx + 1 == np.blocks) {  // npdpc tolerates a bad last entry
                    memset(s.out.data(), 0, np.blockSize);
                    return;
                }
                s.res = Result::fail(Status::Unsupported, "unsupported table entry at block " + std::to_string(idx));
                return;
            }
            uint32_t flags = rd32(e + 0x18);
            if (!(flags & 1) && opt.verify) {
                psp::BBMac mac(3);
                mac.update(s.in.data(), s.size);
                if (!psp::bbmacVerify(mac, e, np.vkey)) {
                    s.res = Result::fail(Status::VerifyFailed, "MAC mismatch at block " + std::to_string(idx));
                    return;
                }
            }
            if (!(flags & 4)) {
                psp::BBCipher c(h + 0xA0, np.vkey, rd32(e + 0x10) >> 4);
                c.update(s.in.data(), s.size);
            }
            if (s.size < np.blockSize) {
                int d = psp::lzrcDecompress(s.out.data(), (int)np.blockSize, s.in.data(), (int)s.size);
                if (d != (int)np.blockSize) {
                    s.res = Result::fail(Status::InvalidInput, "LZRC decompression failed at block " + std::to_string(idx));
                    return;
                }
            } else {
                memcpy(s.out.data(), s.in.data(), np.blockSize);
            }
        });
        if (stop || cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        for (size_t k = 0; k < n; k++) {
            if (!slots[k].res) return slots[k].res;
            uint64_t pos = (uint64_t)(first + k) * np.blockSize;
            if (pos >= isoSize) break;
            if (pos <= 0x8000 && pos + np.blockSize >= 0x8800)
                pvdVolume = psp::isoVolumeSizeFromPvd(slots[k].out.data() + (0x8000 - pos));
            size_t len = (size_t)std::min<uint64_t>(np.blockSize, isoSize - pos);
            if (!out.writeAt(pos, slots[k].out.data(), len)) return Result::fail(Status::IoError, "write failed");
        }
        if (cb.progress) cb.progress(std::min<uint64_t>((uint64_t)(first + n) * np.blockSize, isoSize), isoSize);
    }
    if (opt.trimToVolumeSize && pvdVolume > 0 && pvdVolume < isoSize) {
        logf(cb, "Trimming image to ISO9660 volume size (%llu bytes)", (unsigned long long)pvdVolume);
        isoSize = pvdVolume;
    }
    if (!out.finish(isoSize)) return Result::fail(Status::IoError, "failed to finalize output");
    if (outIsoSize) *outIsoSize = isoSize;
    logf(cb, "ISO size: %llu bytes", (unsigned long long)isoSize);
    return Result::success();
}

// ============================================================================ info

Result readPbpInfo(InputStream& in, PbpInfo& info) {
    NpImage np;
    Result r = parseNp(in, np, true);
    if (!r) return r;
    const uint8_t* h = np.header;
    char buf[0x31] = {0};
    memcpy(buf, h + 0x10, 0x30);
    info.contentId = buf;
    char id[11] = {0};
    memcpy(id, h + 0x70, 10);
    info.discId = id;
    info.npFlags = rd32(h + 0x08);
    info.blockBasis = np.basis;
    info.blocks = np.blocks;
    info.isoSize = np.lbaSize * 0x800;
    memcpy(info.versionKey.data(), np.vkey, 16);
    info.compressed = false;
    for (uint32_t i = 0; i < np.blocks; i++)
        if (rd32(&np.table[(size_t)i * kTableEntry + 0x14]) < np.blockSize) info.compressed = true;
    if (!np.sfo.empty()) {
        psp::Sfo s(np.sfo);
        s.getString("TITLE", info.title);
    }
    return Result::success();
}

Result checkIsoDump(InputStream& iso, IsoDumpCheck& out) {
    out = IsoDumpCheck{};
    out.fileSize = iso.size();
    uint8_t pvd[2048];
    if (out.fileSize < 17 * 2048ull || !iso.readAt(16 * 2048ull, pvd, sizeof(pvd)))
        return Result::fail(Status::InvalidInput, "file is too small to be an ISO image");
    out.volumeSize = psp::isoVolumeSizeFromPvd(pvd);
    if (!out.volumeSize) return Result::fail(Status::InvalidInput, "no ISO9660 primary volume descriptor");
    return Result::success();
}

Result checkIsoDump(const std::string& isoPath, IsoDumpCheck& out) {
    std::string err;
    auto in = openInputFile(isoPath, &err);
    if (!in) return Result::fail(Status::IoError, err);
    return checkIsoDump(*in, out);
}

std::string suggestedIsoName(const PbpInfo& info) {
    std::string id = info.discId;
    if (id.empty() && info.contentId.size() >= 16) id = info.contentId.substr(7, 4) + "-" + info.contentId.substr(11, 5);
    return (id.empty() ? std::string("image") : id) + ".ISO";
}

// ============================================================================ path helpers

namespace {

template <typename Fn>
Result runToPath(InputStream& in, const std::string& outPath, Fn fn) {
    std::string err;
    std::string part = outPath + ".part";
    Result r;
    {
        auto out = openOutputFile(part, &err);
        if (!out) return Result::fail(Status::IoError, err);
        r = fn(in, *out);
    }
    if (!r) {
        detail::removeFile(part);
        return r;
    }
    if (!detail::renameReplace(part, outPath)) {
        detail::removeFile(part);
        return Result::fail(Status::IoError, "cannot rename '" + part + "' to '" + outPath + "'");
    }
    return r;
}

template <typename Fn>
Result runToFile(const std::string& inPath, const std::string& outPath, Fn fn) {
    std::string err;
    auto in = openInputFile(inPath, &err);
    if (!in) return Result::fail(Status::IoError, err);
    return runToPath(*in, outPath, fn);
}

}  // namespace

Result isoToPbp(InputStream& iso, const std::string& pbpPath, const IsoToPbpOptions& opt, const Callbacks& cb) {
    return runToPath(iso, pbpPath, [&](InputStream& i, OutputStream& o) { return isoToPbp(i, o, opt, cb); });
}

Result isoToPbp(const std::string& isoPath, const std::string& pbpPath, const IsoToPbpOptions& opt, const Callbacks& cb) {
    return runToFile(isoPath, pbpPath, [&](InputStream& i, OutputStream& o) { return isoToPbp(i, o, opt, cb); });
}

Result pbpToIso(const std::string& pbpPath, const std::string& isoPath, const PbpToIsoOptions& opt, const Callbacks& cb) {
    return runToFile(pbpPath, isoPath, [&](InputStream& i, OutputStream& o) { return pbpToIso(i, o, opt, cb); });
}

Result readPbpInfo(const std::string& pbpPath, PbpInfo& info) {
    std::string err;
    auto in = openInputFile(pbpPath, &err);
    if (!in) return Result::fail(Status::IoError, err);
    return readPbpInfo(*in, info);
}

}  // namespace ipbp
