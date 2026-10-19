// SPDX-License-Identifier: see LICENSE
//
// iPBP - convert between PSP UMD ISO images and PSN-style EBOOT.PBP (NPUMDIMG) files.
//
// C++17 API. All conversion functions are synchronous but may be cancelled from any
// thread through a CancelToken; cancellation is honoured within one 32 KiB block.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ipbp {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 2;
constexpr int kVersionPatch = 0;

enum class Status {
    Ok = 0,
    Cancelled,
    IoError,         // could not open/read/write a file
    InvalidInput,    // input is not a PSP ISO / not an NPUMDIMG PBP / corrupt
    Unsupported,     // valid but uses features iPBP does not implement
    VerifyFailed,    // MAC/signature check failed while decrypting
};

const char* statusName(Status s);

struct Result {
    Status status = Status::Ok;
    std::string message;
    bool ok() const { return status == Status::Ok; }
    explicit operator bool() const { return ok(); }
    static Result success() { return {}; }
    static Result fail(Status s, std::string msg) { return {s, std::move(msg)}; }
};

// Thread-safe cancellation flag. Copies share the same flag.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { flag_->store(true, std::memory_order_relaxed); }
    void reset() const { flag_->store(false, std::memory_order_relaxed); }
    bool cancelled() const { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

struct Callbacks {
    // Called from the converting thread after each batch of blocks (bytes done / total).
    std::function<void(uint64_t done, uint64_t total)> progress;
    // Informational messages (key, content id, sizes, warnings).
    std::function<void(const std::string& line)> log;
    CancelToken cancel;
};

// ---------------------------------------------------------------- I/O abstraction

// Random-access input. Implement this to feed data from your own virtual file system.
class InputStream {
public:
    virtual ~InputStream() = default;
    virtual uint64_t size() const = 0;
    // Read exactly `len` bytes at `offset`; return false on error / short read.
    virtual bool readAt(uint64_t offset, void* buf, size_t len) = 0;
};

// Random-access output.
class OutputStream {
public:
    virtual ~OutputStream() = default;
    virtual bool writeAt(uint64_t offset, const void* buf, size_t len) = 0;
    // Called once at the end with the final size (lets file streams truncate).
    virtual bool finish(uint64_t finalSize) { (void)finalSize; return true; }
};

std::unique_ptr<InputStream> openInputFile(const std::string& utf8Path, std::string* error = nullptr);
std::unique_ptr<OutputStream> openOutputFile(const std::string& utf8Path, std::string* error = nullptr);

// ---------------------------------------------------------------- ISO -> PBP

struct IsoToPbpOptions {
    // LZRC-compress blocks (only kept when they shrink to <= 90%, like sign_np -c).
    bool compress = true;
    // With compress: search harder for the smallest LZRC (optimal parsing), about 5-10x slower.
    // Still plain LZRC that the PSP decodes, but no longer byte-identical to sign_np's output.
    bool maxCompression = false;
    // Content ID "XXYYYY-AAAABBBBB_CC-DDDDDDDDDDDDDDDD". Empty: "UP0000-<DISC_ID>_00-0000000000000000".
    std::string contentId;
    // Version key. Empty: NPDRM fixed key derived from the content id (NP flags 0x01000003),
    // set: used as-is (NP flags 0x00000002) - same behaviour as sign_np.
    std::optional<std::array<uint8_t, 16>> versionKey;
    // Optional boot screen (STARTDAT). Should be a 480x272 PNG.
    std::vector<uint8_t> startdatPng;
    // Optional ready-made PGD blob appended to DATA.PSP (e.g. an encrypted OPNSSMP).
    std::vector<uint8_t> pgd;
    // Worker threads for compression. 0 = std::thread::hardware_concurrency().
    unsigned threads = 0;
    // When set, the header key / nonces are derived from this value instead of a random
    // source, making the output byte-for-byte reproducible.
    std::optional<uint64_t> deterministicSeed;
};

Result isoToPbp(InputStream& iso, OutputStream& pbp, const IsoToPbpOptions& opt = {}, const Callbacks& cb = {},
                uint64_t* outPbpSize = nullptr);
// Path variant: writes to "<pbpPath>.part" and renames on success; the partial file is
// removed on error or cancellation.
Result isoToPbp(const std::string& isoPath, const std::string& pbpPath, const IsoToPbpOptions& opt = {},
                const Callbacks& cb = {});
// Stream input, path output (the same .part handling).
Result isoToPbp(InputStream& iso, const std::string& pbpPath, const IsoToPbpOptions& opt = {},
                const Callbacks& cb = {});

// ---------------------------------------------------------------- PBP -> ISO

struct PbpToIsoOptions {
    // Verify per-block MACs (detects corruption / wrong keys).
    bool verify = true;
    // Cut the image at the size recorded in the ISO9660 volume descriptor. Off by default:
    // the NPUMDIMG header already gives the exact image length (as used by npdpc), and some
    // images carry real data past the declared volume size.
    bool trimToVolumeSize = false;
    unsigned threads = 0;
};

Result pbpToIso(InputStream& pbp, OutputStream& iso, const PbpToIsoOptions& opt = {}, const Callbacks& cb = {},
                uint64_t* outIsoSize = nullptr);
Result pbpToIso(const std::string& pbpPath, const std::string& isoPath, const PbpToIsoOptions& opt = {},
                const Callbacks& cb = {});

// ---------------------------------------------------------------- inspection

struct PbpInfo {
    std::string contentId;
    std::string discId;        // e.g. "ULUS-10041" (from the NPUMDIMG header)
    std::string title;         // from PARAM.SFO when present
    uint32_t npFlags = 0;
    uint32_t blockBasis = 0;   // sectors per block
    uint32_t blocks = 0;
    uint64_t isoSize = 0;      // from the header LBA range
    std::array<uint8_t, 16> versionKey{};
    bool compressed = false;   // any block LZRC-compressed
};

Result readPbpInfo(InputStream& pbp, PbpInfo& info);
Result readPbpInfo(const std::string& pbpPath, PbpInfo& info);

// ---------------------------------------------------------------- ISO dump check

// Compares the file size with the size recorded in the ISO9660 primary volume descriptor,
// like the "Good dump / Overdump / Underdump" check of the original GUI.
struct IsoDumpCheck {
    uint64_t fileSize = 0;
    uint64_t volumeSize = 0;  // sectors in the PVD * 2048
    bool good() const { return fileSize == volumeSize; }
    int64_t difference() const { return (int64_t)fileSize - (int64_t)volumeSize; }  // > 0: overdump
};

Result checkIsoDump(InputStream& iso, IsoDumpCheck& out);
Result checkIsoDump(const std::string& isoPath, IsoDumpCheck& out);

// Suggested output names, mirroring the original GUI ("ULUS-12345.ISO" / "EBOOT.PBP").
std::string suggestedIsoName(const PbpInfo& info);

}  // namespace ipbp
