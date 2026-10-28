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

// The RCO format, as documented by rcomage (ZiNgA BuRgA, kakaroto) and checked against 6.61's
// flash0 RCOs. Everything is little-endian u32 unless noted.
//
// Header (0xA4 bytes):
//   0x00 'PRF\0' (0x46525000)   0x04 version (0x100+ on 3.50 and later)
//   0x0C compression: high nibble 0 none, 1 zlib, 2 RLZ (see RLZDecoder)
//   0x10 main, vsmx, text, sound, model, image, (unused), font, object, anim table offsets
//   0x38 text data offset, length; 0x40 label data; 0x48 event data
//   0x50 text/image/model/sound/object/anim pointer tables (offset, length each)
//   0x80 image data offset, length; 0x88 sound data; 0x90 model data
// If compressed, at 0xA4: packed length, unpacked length, longest text length, then the packed
// tree. Unpacked, it sits at 0xA4 and every offset above (other than the *data ones) points into
// it as if the file had never been compressed. The text data is compressed separately, one block
// per language: u16 language, u16 1, next block offset, packed length, unpacked length, data.
//
// Tree entries, depth first, each child right after its parent's entry:
//   u8 type, u8 table, u16 0, label offset (into the labels, or -1), header size (0x28),
//   entry size (header + extra, objects only), child count, next sibling offset, previous
//   sibling offset, offset from parent, 2x 0. Then the table-specific extra data:
//   text:   u16 language, u16 format (0 UTF-8, 1 UTF-16, 2 UTF-32), count, then per string
//           label offset, byte length, offset into the language's text data
//   image:  u16 format (5 = GIM), u16 compression, packed size, offset into image data,
//           unpacked size (missing when not compressed and the next entry starts right after)
//   sound:  u16 format (1 = VAG), u16 channels, total size, offset, then size/offset per channel
//           (at least two pairs)
//   font:   u16 format, u16 compression, 2 words
//   vsmx:   offset (0), length, the data itself
//   object: the attributes, one word each, two for a reference (u32 type, u32 tree offset)
//   anim:   the same, for the animation steps

#include <algorithm>
#include <cstring>
#include <memory>
#include "zlib.h"

#include "Common/Data/Encoding/Utf8.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Core/Util/RCOFile.h"

namespace {

const uint32_t RCO_SIGNATURE = 0x46525000;
const uint32_t HEADER_SIZE = 0xA4;
const uint32_t ENTRY_HEADER_SIZE = 0x28;
const uint32_t NULL_PTR = 0xFFFFFFFF;
const uint32_t MAX_UNPACKED = 32 * 1024 * 1024;

enum Compression {
	COMPRESSION_NONE = 0,
	COMPRESSION_ZLIB = 1,
	COMPRESSION_RLZ = 2,
};

uint16_t Read16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t Read32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t AlignTo4(uint32_t v) {
	return (v + 3) & ~3U;
}

// Sony's RLZ, the RCOs' compression: an LZ77 with a range coder, a cousin of the PSP's LZRC
// (Core/FileSystems/tlzrc.cpp) with different contexts. Written as the inverse of rcomage's
// compressor (rlzpack.c), with the probability update LZRC uses, which is what the firmware's data
// decodes with. The first byte is the mode: bits 0-2 shift the literal context, bits 3-4 set the
// starting probability.
class RLZDecoder {
public:
	RLZDecoder(const uint8_t *in, uint32_t inSize) : in_(in), inSize_(inSize) {}

	bool Decode(uint8_t *out, uint32_t outSize) {
		if (inSize_ < 5) {
			return false;
		}
		const uint8_t mode = in_[0];
		const int lc = mode & 7;
		const uint8_t init = (uint8_t)((4 - ((mode >> 3) & 3)) << 5);
		memset(literals_, init, sizeof(literals_));
		memset(distLong_, init, sizeof(distLong_));
		memset(distShort_, init, sizeof(distShort_));
		memset(distances_, init, sizeof(distances_));
		memset(lenFlags_, init, sizeof(lenFlags_));
		memset(lens_, init, sizeof(lens_));
		range_ = 0xFFFFFFFF;
		code_ = ((uint32_t)in_[1] << 24) | ((uint32_t)in_[2] << 16) | ((uint32_t)in_[3] << 8) | in_[4];
		pos_ = 5;

		uint32_t outPos = 0;
		int state = 0;
		uint8_t last = 0;
		while (!error_) {
			// How many bits the match length has, 0 for a literal.
			int lenBits = 0;
			while (lenBits < 8 && Bit(&lenFlags_[lenBits * 8 + state])) {
				lenBits++;
			}

			if (lenBits == 0) {
				const int ctx = (int)((((outPos & 7) << 8) | last) >> lc) & 7;
				int node = 1;
				while (node < 0x100) {
					node = node * 2 + Bit(&literals_[ctx * 0xFF + node - 1]);
				}
				if (outPos >= outSize) {
					return false;
				}
				out[outPos++] = (uint8_t)(node - 0x100);
				if (state > 0) {
					state--;
				}
			} else {
				const int treeCtx = lenBits - 1;
				int len = 1;
				if (lenBits >= 2) {
					const int e = lenBits - 2;
					len = Number(&lens_[((state & 7) + e * 8) * 4 + ((outPos << e) & 3)], e, 1);
				}
				if (len == 0xFF) {
					return outPos == outSize;
				}
				const uint32_t matchLen = len + 1;

				// Number of bits of the distance.
				int node = 1;
				int msb;
				if (matchLen < 4) {
					for (int i = 0; i < 3; i++) {
						node = node * 2 + Bit(&distShort_[node * 8 + treeCtx]);
					}
					msb = node - 8;
				} else {
					for (int i = 0; i < 5; i++) {
						node = node * 2 + Bit(&distLong_[node * 8 + treeCtx]);
					}
					msb = node - 32 - 12;
				}
				uint32_t dist = 1;
				if (msb > 0) {
					if (msb > MAX_DIST_BITS) {
						return false;
					}
					dist = Number(&distances_[(msb - 1) * 0x20], msb - 1, 1);
				}
				if (dist == 0 || dist > outPos || matchLen > outSize - outPos) {
					return false;
				}
				for (uint32_t i = 0; i < matchLen; i++, outPos++) {
					out[outPos] = out[outPos - dist];
				}
				state = 7 - (int)(outPos & 1);
			}
			last = out[outPos - 1];
		}
		return false;
	}

private:
	enum { MAX_DIST_BITS = 20 };

	uint8_t NextByte() {
		if (pos_ >= inSize_) {
			error_ = true;
			return 0;
		}
		return in_[pos_++];
	}

	void Normalize() {
		if (range_ < 0x01000000) {
			range_ <<= 8;
			code_ = (code_ << 8) | NextByte();
		}
	}

	int Bit(uint8_t *prob) {
		const uint32_t bound = (range_ >> 8) * *prob;
		int bit;
		*prob -= *prob >> 3;
		if (code_ < bound) {
			range_ = bound;
			*prob += 31;
			bit = 1;
		} else {
			code_ -= bound;
			range_ -= bound;
			bit = 0;
		}
		Normalize();
		return bit;
	}

	// Bits e..0 of a number whose top bit (above them) is implicit in val. The two highest go
	// through one probability, bits 3 and up past those are direct, the lowest three have their own.
	int Number(uint8_t *probs, int e, int val) {
		if (e >= 3) {
			val = val * 2 + Bit(probs);
			e--;
			if (e >= 3) {
				val = val * 2 + Bit(probs);
				e--;
			}
			probs++;
			if (e >= 3) {
				for (int i = 0; i < e - 2; i++) {
					range_ >>= 1;
					int bit = 0;
					if (code_ < range_) {
						bit = 1;
					} else {
						code_ -= range_;
					}
					val = val * 2 + bit;
				}
				e = 2;
				Normalize();
			}
		}
		for (; e >= 0; e--) {
			val = val * 2 + Bit(probs++);
		}
		return val;
	}

	const uint8_t *in_;
	uint32_t inSize_;
	uint32_t pos_ = 0;
	uint32_t range_ = 0;
	uint32_t code_ = 0;
	bool error_ = false;

	uint8_t literals_[8 * 0xFF];
	uint8_t distLong_[32 * 8];
	uint8_t distShort_[8 * 8];
	uint8_t distances_[(MAX_DIST_BITS + 1) * 0x20];
	uint8_t lenFlags_[8 * 8];
	uint8_t lens_[0x100];
};

bool Decompress(int compression, const uint8_t *in, uint32_t inSize, uint32_t outSize, std::vector<uint8_t> *out) {
	if (outSize > MAX_UNPACKED) {
		return false;
	}
	out->resize(outSize);
	switch (compression) {
	case COMPRESSION_NONE:
		if (inSize < outSize) {
			return false;
		}
		memcpy(out->data(), in, outSize);
		return true;
	case COMPRESSION_ZLIB:
	{
		uLongf destLen = outSize;
		int ret = uncompress(out->data(), &destLen, in, inSize);
		// Sony's zlib streams sometimes end oddly; what's unpacked is still good (rcomage does the same).
		return (ret == Z_OK || ret == Z_DATA_ERROR) && destLen > 0;
	}
	case COMPRESSION_RLZ:
	{
		// About 3.5KB of probabilities, keep it off the stack.
		std::unique_ptr<RLZDecoder> decoder(new RLZDecoder(in, inSize));
		return decoder->Decode(out->data(), outSize);
	}
	default:
		return false;
	}
}

// Attribute kinds per object type, in the order rcomage's objattribdef-psp.ini lists them, indexed
// by object type. f float, i int, u unknown word: one word each. e event, m image, t text, o object,
// r any reference, d model: two words (type, offset).
const char *const OBJECT_ATTRIBUTES[] = {
	"",                                                 // 0x00 object tree
	"ieeee",                                            // 0x01 page
	"fffffffffffffiemi",                                // 0x02 plane
	"fffffffffffffiemmmteeeeeeeei",                     // 0x03 button
	"fffffffffffffieieeeee",                            // 0x04 xmenu
	"umr",                                              // 0x05 xmlist
	"fffffffffffffieieeeeeee",                          // 0x06 xlist
	"fffffffffffffiefurrr",                             // 0x07 progress
	"fffffffffffffieffurrrrr",                          // 0x08 scroll
	"fffffffffffffieiuiffreeeeeeeeee",                  // 0x09 mlist
	"ttr",                                              // 0x0A mitem
	"",                                                 // 0x0B
	"mtr",                                              // 0x0C xitem
	"fffffffffffffietriiffffffffuuuffffffffuuuffffu",   // 0x0D text
	"fffffffffffffied",                                 // 0x0E model
	"fffffffffffffieuirreeerrrrr",                      // 0x0F spin
	"r",                                                // 0x10 action
	"fffffffffffffieiiiiifrreerrrroo",                  // 0x11 itemspin
	"fffffffffffffie",                                  // 0x12 group
	"fffffffffffffieiufrrrrrrer",                       // 0x13 llist
	"trr",                                              // 0x14 litem
	"fffffffffffffieuuuurrerrreeoor",                   // 0x15 edit
	"fffffffffffffieifttrreeeeeeeee",                   // 0x16 clock
	"fffffffffffffiefeeee",                             // 0x17 ilist
	"tt",                                               // 0x18 iitem
	"fffffffffffffiemmm",                               // 0x19 icon
	"fffffffffffffiemeeerrrri",                         // 0x1A ubutton
};

// Animation step sizes in words, by type: 2 move, 3 recolour, 4 rotate, 5 resize, 6 fade,
// 7 delay, 8 fire event, 9 lock, 10 unlock, 11 slide out. 0 is the tree, 1 an animation.
const int ANIM_WORDS[] = { 0, 0, 7, 8, 7, 7, 5, 1, 2, 1, 1, 8 };

}  // namespace

int RCOFile::AttributeWordOffset(uint8_t objType, int attr) {
	if (objType >= ARRAY_SIZE(OBJECT_ATTRIBUTES) || attr < 0) {
		return -1;
	}
	const char *kinds = OBJECT_ATTRIBUTES[objType];
	if (attr >= (int)strlen(kinds)) {
		return -1;
	}
	int words = 0;
	for (int i = 0; i < attr; i++) {
		char k = kinds[i];
		words += (k == 'f' || k == 'i' || k == 'u') ? 1 : 2;
	}
	return words;
}

bool RCOFile::Load(const std::vector<uint8_t> &data) {
	file_.clear();
	tree_.clear();
	text_.clear();
	entries_.clear();

	if (data.size() < HEADER_SIZE || Read32(&data[0]) != RCO_SIGNATURE) {
		WARN_LOG(Log::Loader, "RCO: not an RCO file (or a PS3 one)");
		return false;
	}
	file_ = data;
	if (!ReadTables(data)) {
		entries_.clear();
		tree_.clear();
		text_.clear();
		file_.clear();
		return false;
	}
	return true;
}

bool RCOFile::ReadTables(const std::vector<uint8_t> &file) {
	const uint8_t *h = file.data();
	compression_ = (int)(Read32(h + 0x0C) >> 4);
	const uint32_t mainTable = Read32(h + 0x10);
	const uint32_t textData = Read32(h + 0x38);
	const uint32_t textDataLength = Read32(h + 0x3C);
	labelsOffset_ = Read32(h + 0x40);
	labelsLength_ = Read32(h + 0x44);
	imageDataOffset_ = Read32(h + 0x80);
	imageDataLength_ = Read32(h + 0x84);

	if (compression_ == COMPRESSION_NONE) {
		tree_ = file;
	} else {
		if (file.size() < HEADER_SIZE + 12) {
			return false;
		}
		const uint32_t packed = Read32(h + HEADER_SIZE);
		const uint32_t unpacked = Read32(h + HEADER_SIZE + 4);
		const uint32_t packedStart = HEADER_SIZE + 12;
		if (packed > file.size() - packedStart) {
			WARN_LOG(Log::Loader, "RCO: packed tree runs past the end of the file");
			return false;
		}
		std::vector<uint8_t> tables;
		if (!Decompress(compression_, h + packedStart, packed, unpacked, &tables)) {
			WARN_LOG(Log::Loader, "RCO: can't unpack the tree (compression %d)", compression_);
			return false;
		}
		tree_.assign(h, h + HEADER_SIZE);
		tree_.insert(tree_.end(), tables.begin(), tables.end());

		// The text data, one compressed block per language.
		if (textData != NULL_PTR && textDataLength != 0) {
			uint32_t pos = textData;
			while (pos + 16 <= file.size()) {
				const uint8_t *b = h + pos;
				TextBlock block;
				block.language = Read16(b);
				const uint32_t next = Read32(b + 4);
				const uint32_t blockPacked = Read32(b + 8);
				const uint32_t blockUnpacked = Read32(b + 12);
				if (blockPacked > file.size() - pos - 16) {
					break;
				}
				if (Decompress(compression_, b + 16, blockPacked, blockUnpacked, &block.data)) {
					text_.push_back(std::move(block));
				} else {
					WARN_LOG(Log::Loader, "RCO: can't unpack the text of language %d", block.language);
				}
				if (next == 0) {
					break;
				}
				pos += next;
			}
		}
	}

	uint32_t end = 0;
	if (!ParseEntry(mainTable, -1, 0, &end)) {
		return false;
	}

	if (compression_ == COMPRESSION_NONE && textData != NULL_PTR && textDataLength != 0 && textData + textDataLength <= file.size()) {
		// Uncompressed: all languages share the one block, the indexes carry absolute-ish offsets
		// into it. Give every language the same data.
		for (const Entry &e : entries_) {
			if (e.table == Table::TEXT && e.type == 1 && e.extra.size() >= 2) {
				TextBlock block;
				block.language = Read16(e.extra.data());
				block.data.assign(h + textData, h + textData + textDataLength);
				text_.push_back(std::move(block));
			}
		}
	}
	return true;
}

const uint8_t *RCOFile::TreePtr(uint32_t offset, uint32_t size) const {
	if (offset > tree_.size() || size > tree_.size() - offset) {
		return nullptr;
	}
	return tree_.data() + offset;
}

std::string RCOFile::LabelAt(uint32_t offset) const {
	if (offset == NULL_PTR || offset >= labelsLength_) {
		return std::string();
	}
	const uint8_t *p = TreePtr(labelsOffset_ + offset, 1);
	if (!p) {
		return std::string();
	}
	size_t maxLen = std::min<size_t>(labelsLength_ - offset, tree_.size() - (labelsOffset_ + offset));
	size_t len = strnlen((const char *)p, maxLen);
	return std::string((const char *)p, len);
}

bool RCOFile::ParseEntry(uint32_t pos, int parent, int depth, uint32_t *end) {
	if (depth > 32 || entries_.size() > 65536) {
		return false;
	}
	const uint8_t *p = TreePtr(pos, ENTRY_HEADER_SIZE);
	if (!p) {
		WARN_LOG(Log::Loader, "RCO: entry at %08x is outside the tree", pos);
		return false;
	}

	Entry e;
	e.type = p[0];
	e.table = (Table)p[1];
	e.offset = pos;
	e.label = LabelAt(Read32(p + 4));
	e.parent = parent;
	const uint32_t entrySize = Read32(p + 0x0C);
	const uint32_t childCount = Read32(p + 0x10);
	const uint32_t nextOffset = Read32(p + 0x14);

	// How much extra data follows the header.
	uint32_t extraSize = 0;
	const uint8_t *x = p + ENTRY_HEADER_SIZE;
	const uint32_t xpos = pos + ENTRY_HEADER_SIZE;
	auto have = [&](uint32_t size) { return TreePtr(xpos, size) != nullptr; };
	if (e.type != 0) {
		switch (e.table) {
		case Table::MAIN:
			break;
		case Table::VSMX:
			if (!have(8)) {
				return false;
			}
			extraSize = AlignTo4(8 + Read32(x + 4));
			break;
		case Table::TEXT:
			if (!have(8)) {
				return false;
			}
			extraSize = 8 + Read32(x + 4) * 12;
			break;
		case Table::IMAGE:
		case Table::MODEL:
			if (!have(4)) {
				return false;
			}
			extraSize = 16;
			// Uncompressed entries may leave out the unpacked size.
			if ((Read16(x + 2) & 0xFF) == COMPRESSION_NONE && (nextOffset == 0 || nextOffset < ENTRY_HEADER_SIZE + 16)) {
				extraSize = 12;
			}
			break;
		case Table::SOUND:
			if (!have(4)) {
				return false;
			}
			extraSize = 12 + std::max<uint32_t>(2, Read16(x + 2)) * 8;
			break;
		case Table::FONT:
			extraSize = 12;
			break;
		case Table::OBJECT:
		case Table::ANIM:
			if (entrySize >= ENTRY_HEADER_SIZE) {
				extraSize = entrySize - ENTRY_HEADER_SIZE;
			} else if (e.table == Table::ANIM && e.type < ARRAY_SIZE(ANIM_WORDS) && ANIM_WORDS[e.type] >= 0) {
				extraSize = 4 * ANIM_WORDS[e.type];
			} else if (e.table == Table::OBJECT && e.type < ARRAY_SIZE(OBJECT_ATTRIBUTES)) {
				extraSize = 4 * AttributeWordOffset(e.type, (int)strlen(OBJECT_ATTRIBUTES[e.type]) - 1);
				const char *kinds = OBJECT_ATTRIBUTES[e.type];
				size_t n = strlen(kinds);
				if (n) {
					char k = kinds[n - 1];
					extraSize += (k == 'f' || k == 'i' || k == 'u') ? 4 : 8;
				}
			} else {
				WARN_LOG(Log::Loader, "RCO: can't size entry %08x (table %d, type %d)", pos, (int)e.table, e.type);
				return false;
			}
			break;
		default:
			WARN_LOG(Log::Loader, "RCO: unknown table %d at %08x", (int)e.table, pos);
			return false;
		}
	}
	if (!have(extraSize)) {
		return false;
	}
	e.extra.assign(x, x + extraSize);

	const int index = (int)entries_.size();
	entries_.push_back(std::move(e));
	if (parent >= 0) {
		entries_[parent].children.push_back(index);
	}

	uint32_t childPos = xpos + extraSize;
	for (uint32_t i = 0; i < childCount; i++) {
		uint32_t childEnd = 0;
		if (!ParseEntry(childPos, index, depth + 1, &childEnd)) {
			return false;
		}
		childPos = childEnd;
	}
	*end = childPos;
	return true;
}

int RCOFile::Find(Table table, std::string_view label) const {
	for (size_t i = 0; i < entries_.size(); i++) {
		if (entries_[i].table == table && entries_[i].type != 0 && entries_[i].label == label) {
			return (int)i;
		}
	}
	return -1;
}

int RCOFile::FindByOffset(uint32_t offset) const {
	for (size_t i = 0; i < entries_.size(); i++) {
		if (entries_[i].offset == offset) {
			return (int)i;
		}
	}
	return -1;
}

std::vector<uint8_t> RCOFile::ImageData(std::string_view label) const {
	return ImageData(Find(Table::IMAGE, label));
}

std::vector<uint8_t> RCOFile::ImageData(int entryIndex) const {
	std::vector<uint8_t> out;
	if (entryIndex < 0 || entryIndex >= (int)entries_.size()) {
		return out;
	}
	const Entry &e = entries_[entryIndex];
	if (e.table != Table::IMAGE || e.extra.size() < 12) {
		return out;
	}
	const uint8_t *x = e.extra.data();
	const int compression = Read16(x + 2) & 0xFF;
	const uint32_t packed = Read32(x + 4);
	const uint32_t offset = Read32(x + 8);
	const uint32_t unpacked = e.extra.size() >= 16 ? Read32(x + 12) : packed;
	if (offset > imageDataLength_ || packed > imageDataLength_ - offset) {
		return out;
	}
	const uint32_t start = imageDataOffset_ + offset;
	if (start > file_.size() || packed > file_.size() - start) {
		return out;
	}
	if (!Decompress(compression, file_.data() + start, packed, compression == COMPRESSION_NONE ? packed : unpacked, &out)) {
		WARN_LOG(Log::Loader, "RCO: can't unpack image %s", e.label.c_str());
		out.clear();
	}
	return out;
}

bool RCOFile::HasLanguage(int language) const {
	for (const TextBlock &b : text_) {
		if (b.language == language) {
			return true;
		}
	}
	return false;
}

std::vector<std::string> RCOFile::TextLabels(int language) const {
	std::vector<std::string> labels;
	for (const Entry &e : entries_) {
		if (e.table != Table::TEXT || e.type != 1 || e.extra.size() < 8 || Read16(e.extra.data()) != language) {
			continue;
		}
		const uint32_t count = Read32(e.extra.data() + 4);
		for (uint32_t i = 0; i < count && 8 + (i + 1) * 12 <= e.extra.size(); i++) {
			labels.push_back(LabelAt(Read32(e.extra.data() + 8 + i * 12)));
		}
	}
	return labels;
}

std::string RCOFile::Text(int language, std::string_view label) const {
	for (int pass = 0; pass < 2; pass++) {
		const int lang = pass == 0 ? language : (int)RCO_LANG_ENGLISH;
		if (pass == 1 && language == RCO_LANG_ENGLISH) {
			break;
		}

		const TextBlock *block = nullptr;
		for (const TextBlock &b : text_) {
			if (b.language == lang) {
				block = &b;
				break;
			}
		}
		if (!block) {
			continue;
		}

		for (const Entry &e : entries_) {
			if (e.table != Table::TEXT || e.type != 1 || e.extra.size() < 8) {
				continue;
			}
			const uint8_t *x = e.extra.data();
			if (Read16(x) != lang) {
				continue;
			}
			const int format = Read16(x + 2);
			const uint32_t count = Read32(x + 4);
			for (uint32_t i = 0; i < count && 8 + (i + 1) * 12 <= e.extra.size(); i++) {
				const uint8_t *idx = x + 8 + i * 12;
				if (LabelAt(Read32(idx)) != label) {
					continue;
				}
				const uint32_t len = Read32(idx + 4);
				const uint32_t off = Read32(idx + 8);
				if (off > block->data.size() || len > block->data.size() - off) {
					return std::string();
				}
				const uint8_t *s = block->data.data() + off;
				std::string out;
				if (format == 0) {
					out.assign((const char *)s, strnlen((const char *)s, len));
				} else if (format == 1) {
					for (uint32_t j = 0; j + 1 < len; j += 2) {
						uint32_t c = Read16(s + j);
						if (c == 0) {
							break;
						}
						if (c >= 0xD800 && c < 0xDC00 && j + 3 < len) {
							uint32_t lo = Read16(s + j + 2);
							if (lo >= 0xDC00 && lo < 0xE000) {
								c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
								j += 2;
							}
						}
						out += CodepointToUTF8(c);
					}
				} else if (format == 2) {
					for (uint32_t j = 0; j + 3 < len; j += 4) {
						uint32_t c = Read32(s + j);
						if (c == 0) {
							break;
						}
						out += CodepointToUTF8(c);
					}
				}
				return out;
			}
		}
	}
	return std::string();
}

float RCOFile::ObjectFloat(int entryIndex, int attr) const {
	uint32_t v = ObjectInt(entryIndex, attr);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

uint32_t RCOFile::ObjectInt(int entryIndex, int attr) const {
	if (entryIndex < 0 || entryIndex >= (int)entries_.size()) {
		return 0;
	}
	const Entry &e = entries_[entryIndex];
	if (e.table != Table::OBJECT) {
		return 0;
	}
	int word = AttributeWordOffset(e.type, attr);
	if (word < 0 || (size_t)(word + 1) * 4 > e.extra.size()) {
		return 0;
	}
	return Read32(e.extra.data() + word * 4);
}

int RCOFile::ObjectRef(int entryIndex, int attr, uint32_t *refType) const {
	if (refType) {
		*refType = RCO_REF_NONE;
	}
	if (entryIndex < 0 || entryIndex >= (int)entries_.size()) {
		return -1;
	}
	const Entry &e = entries_[entryIndex];
	if (e.table != Table::OBJECT) {
		return -1;
	}
	int word = AttributeWordOffset(e.type, attr);
	if (word < 0 || (size_t)(word + 2) * 4 > e.extra.size()) {
		return -1;
	}
	const uint32_t type = Read32(e.extra.data() + word * 4);
	const uint32_t ptr = Read32(e.extra.data() + word * 4 + 4);
	if (refType) {
		*refType = type;
	}
	if (type == RCO_REF_NONE || ptr == NULL_PTR) {
		return -1;
	}
	if (type == RCO_REF_TEXT) {
		// A text reference is an index into the text table's strings, not an offset.
		return (int)ptr;
	}
	return FindByOffset(ptr);
}

std::string RCOFile::Dump() const {
	static const char *const tableNames[] = { "?", "main", "vsmx", "text", "image", "model", "sound", "font", "object", "anim" };
	std::string out;
	std::vector<int> depth(entries_.size(), 0);
	for (size_t i = 0; i < entries_.size(); i++) {
		const Entry &e = entries_[i];
		if (e.parent >= 0) {
			depth[i] = depth[e.parent] + 1;
		}
		int t = (int)e.table;
		out += std::string(depth[i] * 2, ' ');
		out += StringFromFormat("%s type=%d @%06x \"%s\" extra=%d\n", t < 10 ? tableNames[t] : "?", e.type, e.offset, e.label.c_str(), (int)e.extra.size());
	}
	return out;
}
