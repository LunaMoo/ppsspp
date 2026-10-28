// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decodes the first image (level 0, frame 0) of a PSP GIM file, the image format of the
// firmware's RCOs, to RGBA8888 (R in the low byte, the PSP's own 8888 layout).
// Handles the 16/32-bit direct formats and 4/8-bit indexed with a palette, swizzled or not.
bool DecodeGIM(const uint8_t *data, size_t size, int *width, int *height, std::vector<uint32_t> *pixels);
