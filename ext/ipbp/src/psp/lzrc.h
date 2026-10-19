// SPDX-License-Identifier: see LICENSE
// LZRC: the LZ77 + adaptive binary range coder used by PSP NPUMDIMG blocks.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ipbp {
namespace psp {

// Decompress. Returns number of bytes written, or -1 on malformed input / overflow.
int lzrcDecompress(uint8_t* out, int outCap, const uint8_t* in, int inLen);

// Compress. Returns compressed size, or -1 if the output buffer is too small.
// Matches are capped so that the stream is decodable by the PSP firmware and the
// original tools (max match 255, max distance < 64 KiB).
int lzrcCompress(uint8_t* out, int outCap, const uint8_t* in, int inLen);

// Same format and limits, chosen by cost under the decoder's model (optimal parsing) instead of
// greedily. Much slower, never larger than lzrcCompress. Blocks lzrcCompress leaves above 95% of
// their size are returned as lzrcCompress made them. The output is still plain LZRC, but no
// longer identical to sign_np's.
int lzrcCompressMax(uint8_t* out, int outCap, const uint8_t* in, int inLen);

}  // namespace psp
}  // namespace ipbp
