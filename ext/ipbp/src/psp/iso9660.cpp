// SPDX-License-Identifier: see LICENSE
#include "iso9660.h"

#include <cctype>
#include <cstring>

#include "../util.h"

namespace ipbp {
namespace psp {

uint64_t isoVolumeSizeFromPvd(const uint8_t* s) {
    if (s[0] != 1 || memcmp(s + 1, "CD001", 5) != 0) return 0;
    return (uint64_t)detail::rd32(s + 80) * 2048ull;
}

bool IsoReader::open(std::string* error) {
    uint8_t pvd[2048];
    if (in_.size() < 17 * 2048ull || !in_.readAt(16 * 2048ull, pvd, sizeof(pvd))) {
        if (error) *error = "file is too small to be an ISO image";
        return false;
    }
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        if (error) *error = "no ISO9660 primary volume descriptor (CSO/CISO and other formats are not supported)";
        return false;
    }
    volumeSectors_ = detail::rd32(pvd + 80);
    const uint8_t* r = pvd + 156;
    root_.lba = detail::rd32(r + 2);
    root_.size = detail::rd32(r + 10);
    root_.dir = true;
    return true;
}

static bool nameEquals(const uint8_t* name, size_t len, const std::string& want) {
    // strip version ";1" and trailing '.'
    size_t n = len;
    for (size_t i = 0; i < len; i++)
        if (name[i] == ';') {
            n = i;
            break;
        }
    if (n > 0 && name[n - 1] == '.') n--;
    if (n != want.size()) return false;
    for (size_t i = 0; i < n; i++)
        if (toupper(name[i]) != toupper((unsigned char)want[i])) return false;
    return true;
}

bool IsoReader::lookup(const std::string& path, Entry& out) {
    Entry cur = root_;
    size_t p = 0;
    std::vector<uint8_t> dir;
    while (p < path.size()) {
        while (p < path.size() && (path[p] == '/' || path[p] == '\\')) p++;
        if (p >= path.size()) break;
        size_t e = path.find_first_of("/\\", p);
        std::string comp = path.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? path.size() : e;
        if (!cur.dir || cur.size > 16 * 1024 * 1024) return false;
        dir.resize(cur.size);
        if (!in_.readAt((uint64_t)cur.lba * 2048, dir.data(), dir.size())) return false;
        bool found = false;
        for (size_t off = 0; off < dir.size();) {
            uint8_t len = dir[off];
            if (len == 0) {  // records do not cross sector boundaries
                off = (off / 2048 + 1) * 2048;
                continue;
            }
            if (len < 33 || off + len > dir.size()) return false;
            const uint8_t* rec = &dir[off];
            uint8_t nlen = rec[32];
            if (33u + nlen > len) return false;
            if (!(nlen == 1 && (rec[33] == 0 || rec[33] == 1)) && nameEquals(rec + 33, nlen, comp)) {
                cur.lba = detail::rd32(rec + 2);
                cur.size = detail::rd32(rec + 10);
                cur.dir = (rec[25] & 2) != 0;
                found = true;
                break;
            }
            off += len;
        }
        if (!found) return false;
    }
    out = cur;
    return true;
}

bool IsoReader::readFile(const std::string& path, std::vector<uint8_t>& out) {
    Entry e;
    if (!lookup(path, e) || e.dir) return false;
    if (e.size > 64u * 1024 * 1024) return false;  // sanity: these are small metadata files
    out.resize(e.size);
    return e.size == 0 || in_.readAt((uint64_t)e.lba * 2048, out.data(), e.size);
}

}  // namespace psp
}  // namespace ipbp
