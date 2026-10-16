// SPDX-License-Identifier: see LICENSE
// PARAM.SFO helpers (in-place editing, as done by sign_np).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ipbp {
namespace psp {

class Sfo {
public:
    explicit Sfo(std::vector<uint8_t>& data) : d_(data) {}
    bool valid() const;
    bool getString(const char* key, std::string& out) const;
    // Overwrites the value bytes in place: copies `len` bytes (the entry's current
    // used length) from a zero-padded copy of `value`. Returns false if key is missing.
    bool setStringInPlace(const char* key, const std::string& value);

private:
    bool find(const char* key, size_t& entry) const;
    std::vector<uint8_t>& d_;
};

}  // namespace psp
}  // namespace ipbp
