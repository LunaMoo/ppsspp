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

// GIM, little-endian ("MIG.00.1PSP"):
//   16-byte file header, then nested blocks, each with a 16-byte header:
//     u16 type, u16 0, u32 size (with header and children), u32 next (= size), u32 data offset (0x10)
//   Types: 0x02 root, 0x03 picture (hold children), 0x04 image, 0x05 palette, 0xFF file info.
//   Image and palette data start with a 0x30+ byte header:
//     0x00 u16 header size   0x04 u16 format   0x06 u16 order (0 linear, 1 swizzled)
//     0x08 u16 width   0x0A u16 height   0x0C u16 bpp   0x0E u16 pitch align   0x10 u16 height align
//     0x18 u32 offset of the level/frame offset table   0x1C u32 pixel start   0x20 u32 pixel end
//     0x2A u16 level count   0x2E u16 frame count
//   All offsets are from the start of that header. Formats: 0 RGBA5650, 1 RGBA5551, 2 RGBA4444,
//   3 RGBA8888, 4 index4, 5 index8, 6 index16, 7 index32 (8-10 DXT, not used by the firmware).
//   Swizzled data is in 16-byte x 8-row blocks, like the GE's swizzled textures.

#include <algorithm>
#include <cstring>

#include "Core/Util/GIMImage.h"

namespace {

uint16_t Read16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t Read32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct GIMPicture {
	const uint8_t *header = nullptr;
	size_t size = 0;  // Bytes available from header on.
};

struct GIMBlocks {
	GIMPicture image;
	GIMPicture palette;
};

void FindBlocks(const uint8_t *p, size_t size, int depth, GIMBlocks *out) {
	size_t pos = 0;
	while (pos + 16 <= size && depth < 8) {
		const uint8_t *b = p + pos;
		const uint16_t type = Read16(b);
		uint32_t blockSize = Read32(b + 4);
		const uint32_t dataOffset = Read32(b + 12);
		if (blockSize < 16 || blockSize > size - pos) {
			blockSize = (uint32_t)(size - pos);
		}
		if (dataOffset >= 16 && dataOffset < blockSize) {
			if (type == 0x02 || type == 0x03) {
				FindBlocks(b + dataOffset, blockSize - dataOffset, depth + 1, out);
			} else if (type == 0x04 && !out->image.header) {
				out->image.header = b + dataOffset;
				out->image.size = blockSize - dataOffset;
			} else if (type == 0x05 && !out->palette.header) {
				out->palette.header = b + dataOffset;
				out->palette.size = blockSize - dataOffset;
			}
		}
		pos += blockSize;
	}
}

struct PictureInfo {
	int format;
	int order;
	int width;
	int height;
	int bpp;
	int pitch;         // Bytes per row as stored.
	int storedHeight;  // Rows as stored.
	const uint8_t *pixels;
	size_t pixelBytes;
};

bool ReadPicture(const GIMPicture &pic, PictureInfo *info) {
	if (!pic.header || pic.size < 0x30) {
		return false;
	}
	const uint8_t *h = pic.header;
	info->format = Read16(h + 0x04);
	info->order = Read16(h + 0x06);
	info->width = Read16(h + 0x08);
	info->height = Read16(h + 0x0A);
	info->bpp = Read16(h + 0x0C);
	int pitchAlign = Read16(h + 0x0E);
	int heightAlign = Read16(h + 0x10);
	const uint32_t indexStart = Read32(h + 0x18);
	const uint32_t pixelStart = Read32(h + 0x1C);

	static const int formatBpp[] = { 16, 16, 16, 32, 4, 8, 16, 32 };
	if (info->format < 0 || info->format > 7 || info->width <= 0 || info->height <= 0 || info->width > 4096 || info->height > 4096) {
		return false;
	}
	if (info->bpp != formatBpp[info->format]) {
		info->bpp = formatBpp[info->format];
	}

	if (info->order == 1) {
		// Swizzled: rows padded to 16 bytes, height to 8 rows.
		pitchAlign = std::max(pitchAlign, 16);
		heightAlign = std::max(heightAlign, 8);
	}
	if (pitchAlign <= 0) {
		pitchAlign = 1;
	}
	if (heightAlign <= 0) {
		heightAlign = 1;
	}
	const int rowBytes = (info->width * info->bpp + 7) / 8;
	info->pitch = (rowBytes + pitchAlign - 1) / pitchAlign * pitchAlign;
	info->storedHeight = (info->height + heightAlign - 1) / heightAlign * heightAlign;

	// Level 0, frame 0: the first entry of the offset table, or the pixel start.
	uint32_t start = pixelStart;
	if (indexStart >= 0x30 && indexStart + 4 <= pic.size) {
		uint32_t first = Read32(h + indexStart);
		if (first >= 0x30 && first < pic.size) {
			start = first;
		}
	}
	if (start < 0x30 || start >= pic.size) {
		return false;
	}
	info->pixels = h + start;
	info->pixelBytes = pic.size - start;
	if ((size_t)info->pitch * info->storedHeight > info->pixelBytes) {
		// Some writers don't pad the last block; allow it as long as the visible rows are there.
		if ((size_t)info->pitch * info->height > info->pixelBytes) {
			return false;
		}
		info->storedHeight = info->height;
	}
	return true;
}

// Linear copy of the stored rows, unswizzling if needed.
std::vector<uint8_t> Linearize(const PictureInfo &info) {
	const size_t total = (size_t)info.pitch * info.storedHeight;
	std::vector<uint8_t> out(total);
	if (info.order != 1) {
		memcpy(out.data(), info.pixels, total);
		return out;
	}
	const int blocksPerRow = info.pitch / 16;
	const uint8_t *src = info.pixels;
	for (int by = 0; by < info.storedHeight / 8; by++) {
		for (int bx = 0; bx < blocksPerRow; bx++) {
			for (int row = 0; row < 8; row++) {
				uint8_t *dst = out.data() + (size_t)(by * 8 + row) * info.pitch + bx * 16;
				memcpy(dst, src, 16);
				src += 16;
			}
		}
	}
	return out;
}

uint32_t ExpandColor(int format, uint32_t c) {
	switch (format) {
	case 0: {  // 5650
		uint32_t r = c & 0x1F, g = (c >> 5) & 0x3F, b = (c >> 11) & 0x1F;
		r = (r << 3) | (r >> 2);
		g = (g << 2) | (g >> 4);
		b = (b << 3) | (b >> 2);
		return r | (g << 8) | (b << 16) | 0xFF000000;
	}
	case 1: {  // 5551
		uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
		r = (r << 3) | (r >> 2);
		g = (g << 3) | (g >> 2);
		b = (b << 3) | (b >> 2);
		return r | (g << 8) | (b << 16) | ((c & 0x8000) ? 0xFF000000 : 0);
	}
	case 2: {  // 4444
		uint32_t r = c & 0xF, g = (c >> 4) & 0xF, b = (c >> 8) & 0xF, a = (c >> 12) & 0xF;
		return (r * 0x11) | ((g * 0x11) << 8) | ((b * 0x11) << 16) | ((a * 0x11) << 24);
	}
	default:  // 8888
		return c;
	}
}

uint32_t ReadTexel(const uint8_t *row, int x, int bpp) {
	switch (bpp) {
	case 4: return (row[x >> 1] >> ((x & 1) * 4)) & 0xF;
	case 8: return row[x];
	case 16: return Read16(row + x * 2);
	default: return Read32(row + x * 4);
	}
}

}  // namespace

bool DecodeGIM(const uint8_t *data, size_t size, int *width, int *height, std::vector<uint32_t> *pixels) {
	if (size < 16 + 16 || memcmp(data, "MIG.00.1PSP", 11) != 0) {
		return false;
	}

	GIMBlocks blocks;
	FindBlocks(data + 16, size - 16, 0, &blocks);

	PictureInfo image;
	if (!ReadPicture(blocks.image, &image)) {
		return false;
	}

	std::vector<uint32_t> palette;
	const bool indexed = image.format >= 4;
	if (indexed) {
		PictureInfo pal;
		if (!ReadPicture(blocks.palette, &pal) || pal.format > 3) {
			return false;
		}
		std::vector<uint8_t> linear = Linearize(pal);
		// The palette is one row (or more, for big ones) of colors.
		const int count = pal.width * pal.height;
		palette.resize(count);
		for (int i = 0; i < count; i++) {
			const int x = i % pal.width, y = i / pal.width;
			palette[i] = ExpandColor(pal.format, ReadTexel(linear.data() + (size_t)y * pal.pitch, x, pal.bpp));
		}
	}

	std::vector<uint8_t> linear = Linearize(image);
	pixels->resize((size_t)image.width * image.height);
	for (int y = 0; y < image.height; y++) {
		const uint8_t *row = linear.data() + (size_t)y * image.pitch;
		uint32_t *dst = pixels->data() + (size_t)y * image.width;
		for (int x = 0; x < image.width; x++) {
			uint32_t t = ReadTexel(row, x, image.bpp);
			if (indexed) {
				dst[x] = t < palette.size() ? palette[t] : 0;
			} else {
				dst[x] = ExpandColor(image.format, t);
			}
		}
	}
	*width = image.width;
	*height = image.height;
	return true;
}
