// SPDX-License-Identifier: see LICENSE
#include "sfo.h"

#include <cstring>

#include "../util.h"

namespace ipbp {
namespace psp {

using detail::rd16;
using detail::rd32;

bool Sfo::valid() const {
    if (d_.size() < 20 || memcmp(d_.data(), "\0PSF", 4) != 0) return false;
    uint32_t kt = rd32(&d_[8]), dt = rd32(&d_[12]), n = rd32(&d_[16]);
    return kt <= d_.size() && dt <= d_.size() && 20ull + 16ull * n <= d_.size();
}

bool Sfo::find(const char* key, size_t& entry) const {
    if (!valid()) return false;
    uint32_t kt = rd32(&d_[8]), n = rd32(&d_[16]);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = 20 + 16 * i;
        size_t ko = kt + rd16(&d_[e]);
        if (ko >= d_.size()) continue;
        size_t kl = strnlen(reinterpret_cast<const char*>(&d_[ko]), d_.size() - ko);
        if (kl == strlen(key) && memcmp(&d_[ko], key, kl) == 0) {
            entry = e;
            return true;
        }
    }
    return false;
}

bool Sfo::getString(const char* key, std::string& out) const {
    size_t e;
    if (!find(key, e)) return false;
    uint32_t len = rd32(&d_[e + 4]);
    size_t off = rd32(&d_[12]) + rd32(&d_[e + 12]);
    if (off + len > d_.size()) return false;
    out.assign(reinterpret_cast<const char*>(&d_[off]), len);
    size_t z = out.find('\0');
    if (z != std::string::npos) out.resize(z);
    return true;
}

bool Sfo::setStringInPlace(const char* key, const std::string& value) {
    size_t e;
    if (!find(key, e)) return false;
    uint32_t len = rd32(&d_[e + 4]);
    size_t off = rd32(&d_[12]) + rd32(&d_[e + 12]);
    if (off + len > d_.size()) return false;
    std::vector<uint8_t> src(len + 1, 0);
    memcpy(src.data(), value.data(), value.size() < len ? value.size() : len);
    memcpy(&d_[off], src.data(), len);
    return true;
}

}  // namespace psp
}  // namespace ipbp
