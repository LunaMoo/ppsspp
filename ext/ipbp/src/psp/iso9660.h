// SPDX-License-Identifier: see LICENSE
// Just enough ISO9660 to pull files (PARAM.SFO, icons...) out of a UMD image.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../include/ipbp/ipbp.hpp"

namespace ipbp {
namespace psp {

class IsoReader {
public:
    explicit IsoReader(InputStream& in) : in_(in) {}
    // Validates the primary volume descriptor.
    bool open(std::string* error);
    // Volume size in bytes as declared by the PVD.
    uint64_t volumeSize() const { return volumeSectors_ * 2048ull; }
    // Reads a whole file, path like "/PSP_GAME/PARAM.SFO" (case-insensitive). Returns false if missing.
    bool readFile(const std::string& path, std::vector<uint8_t>& out);

private:
    struct Entry {
        uint32_t lba = 0, size = 0;
        bool dir = false;
    };
    bool lookup(const std::string& path, Entry& e);
    InputStream& in_;
    Entry root_;
    uint64_t volumeSectors_ = 0;
};

// Volume size from a raw PVD sector (sector 16), 0 if not a valid PVD.
uint64_t isoVolumeSizeFromPvd(const uint8_t* sector16);

}  // namespace psp
}  // namespace ipbp
