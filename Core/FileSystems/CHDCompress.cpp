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

// The CHD writing side of MAME's src/lib/util (chd.cpp, chdcodec.cpp, huffman.cpp, bitstream.h,
// flac.cpp, hashing.cpp: license BSD-3-Clause, copyright-holders Aaron Giles), cut down to what
// "chdman createdvd" does and ported to C++17. Every choice that shows in the file (codec settings,
// trial order, tie breaks, the map's encoding) follows MAME, so the output is the same.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Common/Crypto/sha1.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/FileSystems/CHDCompress.h"

#ifdef SHARED_ZLIB
#include <zlib.h>
#else
#include "ext/zlib/zlib.h"
#endif
#include <zstd.h>

#ifndef Z7_ST
#define Z7_ST
#endif
#include "ext/libchdr/deps/lzma-24.05/include/LzmaEnc.h"

#ifndef FLAC__NO_DLL
#define FLAC__NO_DLL
#endif
#include "ext/flac/include/FLAC/stream_encoder.h"

namespace CHDCompress {

namespace {

const uint32_t UNIT_BYTES = 2048;
const uint32_t V5_HEADER_SIZE = 124;
const uint32_t METADATA_HEADER_SIZE = 16;
// Hunks read and compressed per round, at most.
const size_t BATCH_BYTES = 32 * 1024 * 1024;
// chd_file_compressor's ring of hunks being read (see the end of the image in Compress).
const uint32_t WORK_BUFFER_HUNKS = 256;

constexpr uint32_t Tag(char a, char b, char c, char d) {
	return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)c << 8) | (uint32_t)(uint8_t)d;
}

const uint32_t CODEC_ZLIB = Tag('z', 'l', 'i', 'b');
const uint32_t CODEC_ZSTD = Tag('z', 's', 't', 'd');
const uint32_t CODEC_LZMA = Tag('l', 'z', 'm', 'a');
const uint32_t CODEC_HUFFMAN = Tag('h', 'u', 'f', 'f');
const uint32_t CODEC_FLAC = Tag('f', 'l', 'a', 'c');
const uint32_t DVD_METADATA_TAG = Tag('D', 'V', 'D', ' ');
const uint8_t MDFLAGS_CHECKSUM = 0x01;

// Map entry types (chd.cpp).
enum {
	COMPRESSION_TYPE_0 = 0,
	COMPRESSION_TYPE_1 = 1,
	COMPRESSION_TYPE_2 = 2,
	COMPRESSION_TYPE_3 = 3,
	COMPRESSION_NONE = 4,
	COMPRESSION_SELF = 5,
	COMPRESSION_PARENT = 6,
	COMPRESSION_RLE_SMALL,
	COMPRESSION_RLE_LARGE,
	COMPRESSION_SELF_0,
	COMPRESSION_SELF_1,
	COMPRESSION_PARENT_SELF,
	COMPRESSION_PARENT_0,
	COMPRESSION_PARENT_1,
};

void PutU16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
void PutU24(uint8_t *p, uint32_t v) { p[0] = v >> 16; p[1] = v >> 8; p[2] = (uint8_t)v; }
void PutU32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
void PutU48(uint8_t *p, uint64_t v) { PutU16(p, (uint16_t)(v >> 32)); PutU32(p + 2, (uint32_t)v); }
void PutU64(uint8_t *p, uint64_t v) { PutU32(p, (uint32_t)(v >> 32)); PutU32(p + 4, (uint32_t)v); }
uint32_t GetU24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
uint16_t GetU16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint64_t GetU48(const uint8_t *p) { return ((uint64_t)GetU16(p) << 32) | ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5]; }

uint8_t BitWidth(uint64_t v) {
	uint8_t bits = 0;
	while (v) {
		bits++;
		v >>= 1;
	}
	return bits;
}

// CRC-16-CCITT, initial value 0xffff (hashing.cpp's crc16_creator).
struct Crc16Table {
	uint16_t table[256];
	Crc16Table() {
		for (int i = 0; i < 256; i++) {
			uint16_t crc = (uint16_t)(i << 8);
			for (int b = 0; b < 8; b++) {
				crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
			}
			table[i] = crc;
		}
	}
};

uint16_t Crc16(const uint8_t *data, size_t length) {
	static const Crc16Table t;
	uint16_t crc = 0xffff;
	while (length-- != 0) {
		crc = (uint16_t)((crc << 8) ^ t.table[(crc >> 8) ^ *data++]);
	}
	return crc;
}

struct Sha1 {
	uint8_t raw[20];
	bool operator==(const Sha1 &other) const { return memcmp(raw, other.raw, sizeof(raw)) == 0; }
};

class Sha1Creator {
public:
	Sha1Creator() { sha1_starts(&ctx_); }
	void Append(const void *data, size_t length) {
		const uint8_t *p = (const uint8_t *)data;
		while (length > 0) {
			const int chunk = (int)std::min<size_t>(length, 1 << 30);
			sha1_update(&ctx_, (unsigned char *)p, chunk);
			p += chunk;
			length -= chunk;
		}
	}
	Sha1 Finish() {
		Sha1 result;
		sha1_finish(&ctx_, result.raw);
		return result;
	}
	static Sha1 Simple(const void *data, size_t length) {
		Sha1Creator c;
		c.Append(data, length);
		return c.Finish();
	}

private:
	sha1_context ctx_;
};

// bitstream.h's bitstream_out.
class BitstreamOut {
public:
	BitstreamOut(uint8_t *dest, uint32_t length) : write_(dest), dlength_(length) {}

	bool Overflow() const { return doffset_ > dlength_; }

	void Write(uint32_t newbits, int numbits) {
		if (numbits <= 0) {
			return;
		}
		newbits <<= 32 - numbits;
		// Flush the buffer if we're going to overflow it.
		while (bits_ + numbits >= 32 && numbits > 0) {
			while (bits_ >= 8) {
				if (doffset_ < dlength_)
					write_[doffset_] = buffer_ >> 24;
				doffset_++;
				buffer_ <<= 8;
				bits_ -= 8;
			}
			// Offload more bits if it'll still overflow the buffer.
			if (bits_ + numbits >= 32) {
				const int rem = std::min(32 - bits_, numbits);
				buffer_ |= newbits >> bits_;
				bits_ += rem;
				newbits <<= rem;
				numbits -= rem;
			}
		}
		if (numbits <= 0) {
			return;
		}
		buffer_ |= newbits >> bits_;
		bits_ += numbits;
	}

	uint32_t Flush() {
		while (bits_ > 0) {
			if (doffset_ < dlength_)
				write_[doffset_] = buffer_ >> 24;
			doffset_++;
			buffer_ <<= 8;
			bits_ -= 8;
		}
		bits_ = 0;
		buffer_ = 0;
		return doffset_;
	}

private:
	uint32_t buffer_ = 0;
	int bits_ = 0;
	uint8_t *write_;
	uint32_t doffset_ = 0;
	uint32_t dlength_;
};

// huffman.cpp's encoder.
template <int NumCodes, int MaxBits>
class HuffmanEncoder {
public:
	struct Node {
		Node *parent;
		uint32_t count;
		uint32_t weight;
		uint32_t bits;
		uint8_t numbits;
	};

	HuffmanEncoder() { HistoReset(); }

	void HistoReset() { memset(histo_, 0, sizeof(histo_)); }
	void HistoOne(uint32_t data) { histo_[data]++; }
	void EncodeOne(BitstreamOut &bitbuf, uint32_t data) {
		const Node &node = nodes_[data];
		bitbuf.Write(node.bits, node.numbits);
	}

	bool ComputeTreeFromHisto() {
		uint32_t sdatacount = 0;
		for (int i = 0; i < NumCodes; i++)
			sdatacount += histo_[i];

		// Binary search for the best weighting that fits in MaxBits.
		uint32_t lowerweight = 0;
		uint32_t upperweight = sdatacount * 2;
		while (true) {
			const uint32_t curweight = (upperweight + lowerweight) / 2;
			const int curmaxbits = BuildTree(sdatacount, curweight);
			if (curmaxbits <= MaxBits) {
				lowerweight = curweight;
				if (curweight == sdatacount || (upperweight - lowerweight) <= 1)
					break;
			} else {
				upperweight = curweight;
			}
		}
		return AssignCanonicalCodes();
	}

	bool ExportTreeRle(BitstreamOut &bitbuf) {
		int numbits;
		if (MaxBits >= 16)
			numbits = 5;
		else if (MaxBits >= 8)
			numbits = 4;
		else
			numbits = 3;

		int lastval = ~0;
		int repcount = 0;
		for (int curcode = 0; curcode < NumCodes; curcode++) {
			const int newval = nodes_[curcode].numbits;
			if (newval == lastval) {
				repcount++;
			} else {
				if (repcount != 0)
					WriteRleTreeBits(bitbuf, lastval, repcount, numbits);
				lastval = newval;
				repcount = 1;
			}
		}
		WriteRleTreeBits(bitbuf, lastval, repcount, numbits);
		return !bitbuf.Overflow();
	}

	bool ExportTreeHuffman(BitstreamOut &bitbuf) {
		// First RLE compress the lengths of all the nodes.
		std::vector<uint8_t> rleData(NumCodes);
		uint8_t *dest = rleData.data();
		std::vector<uint16_t> rleLengths(NumCodes / 3);
		uint16_t *lengths = rleLengths.data();
		int last = ~0;
		int repcount = 0;

		// A small Huffman context makes a tree for them (ignoring the RLE lengths).
		HuffmanEncoder<24, 6> smallhuff;

		for (int curcode = 0; curcode < NumCodes; curcode++) {
			const int newval = nodes_[curcode].numbits;
			if (newval != last && repcount > 0) {
				if (repcount == 1) {
					smallhuff.HistoOne(*dest++ = (uint8_t)(last + 1));
				} else {
					smallhuff.HistoOne(*dest++ = 0);
					*lengths++ = (uint16_t)(repcount - 2);
				}
			}
			if (newval == last) {
				repcount++;
			} else {
				smallhuff.HistoOne(*dest++ = (uint8_t)(newval + 1));
				last = newval;
				repcount = 0;
			}
		}
		if (repcount > 0) {
			if (repcount == 1) {
				smallhuff.HistoOne(*dest++ = (uint8_t)(last + 1));
			} else {
				smallhuff.HistoOne(*dest++ = 0);
				*lengths++ = (uint16_t)(repcount - 2);
			}
		}

		smallhuff.ComputeTreeFromHisto();

		// The first and last non-zero nodes.
		int firstNonZero = 31, lastNonZero = 0;
		for (int index = 1; index < 24; index++) {
			if (smallhuff.nodes_[index].numbits != 0) {
				if (firstNonZero == 31)
					firstNonZero = index;
				lastNonZero = index;
			}
		}
		firstNonZero = std::min(firstNonZero, 8);

		// The lengths of each small tree node: the RLE token (0), the first non-zero value, then the
		// data terminated by a 7.
		bitbuf.Write(smallhuff.nodes_[0].numbits, 3);
		bitbuf.Write(firstNonZero - 1, 3);
		for (int index = firstNonZero; index <= lastNonZero; index++)
			bitbuf.Write(smallhuff.nodes_[index].numbits, 3);
		bitbuf.Write(7, 3);

		// The maximum length of an RLE count.
		uint32_t temp = NumCodes - 9;
		uint8_t rlefullbits = 0;
		while (temp != 0) {
			temp >>= 1;
			rlefullbits++;
		}

		lengths = rleLengths.data();
		for (uint8_t *src = rleData.data(); src < dest; src++) {
			const uint8_t data = *src;
			smallhuff.EncodeOne(bitbuf, data);
			if (data == 0) {
				const int count = *lengths++;
				if (count < 7) {
					bitbuf.Write(count, 3);
				} else {
					bitbuf.Write(7, 3);
					bitbuf.Write(count - 7, rlefullbits);
				}
			}
		}
		return !bitbuf.Overflow();
	}

	Node nodes_[NumCodes * 2];

private:
	void WriteRleTreeBits(BitstreamOut &bitbuf, int value, int repcount, int numbits) {
		while (repcount > 0) {
			if (value == 1) {
				// 1 is the escape code, so write it twice.
				bitbuf.Write(1, numbits);
				bitbuf.Write(1, numbits);
				repcount--;
			} else if (repcount <= 2) {
				bitbuf.Write(value, numbits);
				repcount--;
			} else {
				const int curReps = std::min(repcount - 3, (1 << numbits) - 1);
				bitbuf.Write(1, numbits);
				bitbuf.Write(value, numbits);
				bitbuf.Write(curReps, numbits);
				repcount -= curReps + 3;
			}
		}
	}

	static int TreeNodeCompare(const void *item1, const void *item2) {
		const Node *node1 = *(const Node *const *)item1;
		const Node *node2 = *(const Node *const *)item2;
		if (node2->weight != node1->weight)
			return node2->weight - node1->weight;
		return (int)node1->bits - (int)node2->bits;
	}

	int BuildTree(uint32_t totaldata, uint32_t totalweight) {
		// A list of all non-zero nodes.
		std::vector<Node *> list(NumCodes * 2);
		int listitems = 0;
		memset(nodes_, 0, NumCodes * sizeof(nodes_[0]));
		for (int curcode = 0; curcode < NumCodes; curcode++) {
			if (histo_[curcode] != 0) {
				list[listitems++] = &nodes_[curcode];
				nodes_[curcode].count = histo_[curcode];
				nodes_[curcode].bits = curcode;
				// Scale the weight by the current effective length, ensuring we don't go to 0.
				nodes_[curcode].weight = (uint32_t)((uint64_t)histo_[curcode] * (uint64_t)totalweight / (uint64_t)totaldata);
				if (nodes_[curcode].weight == 0)
					nodes_[curcode].weight = 1;
			}
		}

		// Largest weight first. The code number breaks ties, so the sort's stability doesn't matter.
		qsort(list.data(), listitems, sizeof(list[0]), TreeNodeCompare);

		int nextalloc = NumCodes;
		while (listitems > 1) {
			// Remove the lowest two items.
			Node &node1 = *list[--listitems];
			Node &node0 = *list[--listitems];

			Node &newnode = nodes_[nextalloc++];
			newnode.parent = nullptr;
			node0.parent = node1.parent = &newnode;
			newnode.weight = node0.weight + node1.weight;

			// Insert into the list at the right place.
			int curitem;
			for (curitem = 0; curitem < listitems; curitem++) {
				if (newnode.weight > list[curitem]->weight) {
					memmove(&list[curitem + 1], &list[curitem], (listitems - curitem) * sizeof(list[0]));
					break;
				}
			}
			list[curitem] = &newnode;
			listitems++;
		}

		// The number of bits in each code.
		int maxbits = 0;
		for (int curcode = 0; curcode < NumCodes; curcode++) {
			Node &node = nodes_[curcode];
			node.numbits = 0;
			node.bits = 0;
			if (node.weight > 0) {
				for (Node *curnode = &node; curnode->parent != nullptr; curnode = curnode->parent)
					node.numbits++;
				if (node.numbits == 0)
					node.numbits = 1;
				maxbits = std::max(maxbits, (int)node.numbits);
			}
		}
		return maxbits;
	}

	bool AssignCanonicalCodes() {
		uint32_t bithisto[33] = {};
		for (int curcode = 0; curcode < NumCodes; curcode++) {
			const Node &node = nodes_[curcode];
			if (node.numbits > MaxBits)
				return false;
			if (node.numbits <= 32)
				bithisto[node.numbits]++;
		}

		// The first code number for each code length.
		uint32_t curstart = 0;
		for (int codelen = 32; codelen > 0; codelen--) {
			const uint32_t nextstart = (curstart + bithisto[codelen]) >> 1;
			if (codelen != 1 && nextstart * 2 != (curstart + bithisto[codelen]))
				return false;
			bithisto[codelen] = curstart;
			curstart = nextstart;
		}

		for (int curcode = 0; curcode < NumCodes; curcode++) {
			Node &node = nodes_[curcode];
			if (node.numbits > 0)
				node.bits = bithisto[node.numbits]++;
		}
		return true;
	}

	uint32_t histo_[NumCodes];

	template <int, int> friend class HuffmanEncoder;
};

// A hunk compressor: returns the compressed size, or 0 when it isn't smaller than srclen.
class Codec {
public:
	virtual ~Codec() {}
	virtual uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) = 0;
};

class ZlibCodec : public Codec {
public:
	ZlibCodec() {
		ok_ = deflateInit2(&z_, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
	}
	~ZlibCodec() {
		if (ok_)
			deflateEnd(&z_);
	}
	uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) override {
		if (!ok_)
			return 0;
		z_.next_in = (Bytef *)src;
		z_.avail_in = srclen;
		z_.total_in = 0;
		z_.next_out = dest;
		z_.avail_out = srclen;
		z_.total_out = 0;
		if (deflateReset(&z_) != Z_OK)
			return 0;
		const int zerr = deflate(&z_, Z_FINISH);
		if (zerr != Z_STREAM_END || z_.total_out >= srclen)
			return 0;
		return (uint32_t)z_.total_out;
	}

private:
	z_stream z_{};
	bool ok_ = false;
};

class ZstdCodec : public Codec {
public:
	ZstdCodec() : stream_(ZSTD_createCStream()) {}
	~ZstdCodec() {
		ZSTD_freeCStream(stream_);
	}
	uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) override {
		if (!stream_ || ZSTD_isError(ZSTD_initCStream(stream_, ZSTD_maxCLevel())))
			return 0;
		ZSTD_inBuffer input{ src, srclen, 0 };
		ZSTD_outBuffer output{ dest, srclen, 0 };
		while (output.pos < output.size) {
			const size_t result = ZSTD_compressStream2(stream_, &output, &input, ZSTD_e_end);
			if (ZSTD_isError(result))
				return 0;
			if (!result)
				break;
		}
		if (output.pos == output.size)
			return 0;
		return (uint32_t)output.pos;
	}

private:
	ZSTD_CStream *stream_;
};

void *LzmaAlloc(ISzAllocPtr, size_t size) { return malloc(size); }
void LzmaFree(ISzAllocPtr, void *address) { free(address); }
const ISzAlloc g_lzmaAlloc = { LzmaAlloc, LzmaFree };

class LzmaCodec : public Codec {
public:
	explicit LzmaCodec(uint32_t hunkbytes) {
		LzmaEncProps_Init(&props_);
		props_.level = 6;
		props_.reduceSize = hunkbytes;
		LzmaEncProps_Normalize(&props_);
	}
	uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) override {
		CLzmaEncHandle encoder = LzmaEnc_Create(&g_lzmaAlloc);
		if (!encoder)
			return 0;
		SizeT complen = srclen;
		SRes res = LzmaEnc_SetProps(encoder, &props_);
		if (res == SZ_OK)
			res = LzmaEnc_MemEncode(encoder, dest, &complen, src, srclen, 0, nullptr, &g_lzmaAlloc, &g_lzmaAlloc);
		LzmaEnc_Destroy(encoder, &g_lzmaAlloc, &g_lzmaAlloc);
		if (res != SZ_OK || complen >= srclen)
			return 0;
		return (uint32_t)complen;
	}

private:
	CLzmaEncProps props_;
};

class HuffmanCodec : public Codec {
public:
	uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) override {
		HuffmanEncoder<256, 16> &e = *encoder_;
		e.HistoReset();
		for (uint32_t cur = 0; cur < srclen; cur++)
			e.HistoOne(src[cur]);
		if (!e.ComputeTreeFromHisto())
			return 0;
		BitstreamOut bitbuf(dest, srclen);
		if (!e.ExportTreeHuffman(bitbuf))
			return 0;
		for (uint32_t cur = 0; cur < srclen; cur++)
			e.EncodeOne(bitbuf, src[cur]);
		const uint32_t complen = bitbuf.Flush();
		if (bitbuf.Overflow() || complen >= srclen)
			return 0;
		return complen;
	}

private:
	std::unique_ptr<HuffmanEncoder<256, 16>> encoder_ = std::make_unique<HuffmanEncoder<256, 16>>();
};

// flac.cpp's flac_encoder writing to memory, with the metadata stripped.
class FlacEncoder {
public:
	FlacEncoder(uint32_t blockSize) : blockSize_(blockSize), encoder_(FLAC__stream_encoder_new()) {}
	~FlacEncoder() {
		if (encoder_)
			FLAC__stream_encoder_delete(encoder_);
	}

	bool Reset(uint8_t *buffer, uint32_t length) {
		if (!encoder_)
			return false;
		start_ = buffer;
		length_ = length;
		offset_ = 0;
		ignoreBytes_ = 4;  // "fLaC"
		foundAudio_ = false;
		// Configured on every reset; if not, results are NOT consistent (says MAME).
		FLAC__stream_encoder_set_verify(encoder_, false);
		FLAC__stream_encoder_set_compression_level(encoder_, 8);
		FLAC__stream_encoder_set_channels(encoder_, 2);
		FLAC__stream_encoder_set_bits_per_sample(encoder_, 16);
		FLAC__stream_encoder_set_sample_rate(encoder_, 44100);
		FLAC__stream_encoder_set_total_samples_estimate(encoder_, 0);
		FLAC__stream_encoder_set_streamable_subset(encoder_, false);
		FLAC__stream_encoder_set_blocksize(encoder_, blockSize_);
		return FLAC__stream_encoder_init_stream(encoder_, &WriteCallback, nullptr, nullptr, nullptr, this) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
	}

	bool EncodeInterleaved(const uint8_t *src, uint32_t samplesPerChannel, bool bigEndian) {
		const int numChannels = 2;
		uint32_t srcindex = 0;
		while (samplesPerChannel != 0) {
			// In batches of 2K samples.
			FLAC__int32 converted[2048];
			FLAC__int32 *dest = converted;
			const uint32_t curSamples = std::min<uint32_t>(2048 / numChannels, samplesPerChannel);
			for (uint32_t s = 0; s < curSamples; s++) {
				for (int c = 0; c < numChannels; c++, srcindex++) {
					const uint8_t *p = src + srcindex * 2;
					*dest++ = (int16_t)(bigEndian ? ((p[0] << 8) | p[1]) : ((p[1] << 8) | p[0]));
				}
			}
			if (!FLAC__stream_encoder_process_interleaved(encoder_, converted, curSamples))
				return false;
			samplesPerChannel -= curSamples;
		}
		return true;
	}

	uint32_t Finish() {
		FLAC__stream_encoder_finish(encoder_);
		return offset_;
	}

private:
	static FLAC__StreamEncoderWriteStatus WriteCallback(const FLAC__StreamEncoder *, const FLAC__byte buffer[], size_t bytes, unsigned, unsigned, void *clientData) {
		FlacEncoder *self = (FlacEncoder *)clientData;
		size_t offset = 0;
		while (offset < bytes) {
			if (self->ignoreBytes_ != 0) {
				const size_t ignore = std::min(bytes - offset, (size_t)self->ignoreBytes_);
				offset += ignore;
				self->ignoreBytes_ -= (uint32_t)ignore;
			} else if (!self->foundAudio_) {
				// A metadata block header: the last-block flag and the length to skip.
				if (bytes - offset < 4)
					return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
				self->foundAudio_ = (buffer[offset] & 0x80) != 0;
				self->ignoreBytes_ = GetU24(&buffer[offset + 1]);
				offset += 4;
			} else {
				// Audio. Like MAME, this copies from the start of the buffer; libFLAC never hands
				// audio and metadata over in one call.
				const size_t count = bytes - offset;
				if (self->offset_ + count <= self->length_)
					memcpy(self->start_ + self->offset_, buffer, count);
				self->offset_ += (uint32_t)count;
				offset += count;
			}
		}
		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	}

	uint32_t blockSize_;
	FLAC__StreamEncoder *encoder_;
	uint8_t *start_ = nullptr;
	uint32_t length_ = 0;
	uint32_t offset_ = 0;
	uint32_t ignoreBytes_ = 0;
	bool foundAudio_ = false;
};

// The data as 16-bit stereo samples, whichever byte order compresses better (a prefix byte says).
class FlacCodec : public Codec {
public:
	explicit FlacCodec(uint32_t hunkbytes) : hunkbytes_(hunkbytes), encoder_(BlockSize(hunkbytes)) {}

	uint32_t Compress(const uint8_t *src, uint32_t srclen, uint8_t *dest) override {
		// Big-endian first, then little-endian.
		if (!encoder_.Reset(dest + 1, hunkbytes_ - 1) || !encoder_.EncodeInterleaved(src, srclen / 4, true))
			return 0;
		const uint32_t complenBE = encoder_.Finish();
		if (!encoder_.Reset(dest + 1, hunkbytes_ - 1) || !encoder_.EncodeInterleaved(src, srclen / 4, false))
			return 0;
		const uint32_t complenLE = encoder_.Finish();

		const uint32_t complen = std::min(complenLE, complenBE);
		if (complen + 1 >= hunkbytes_)
			return 0;
		dest[0] = 'L';
		if (complen != complenLE) {
			// Big-endian was better: redo it.
			dest[0] = 'B';
			if (!encoder_.Reset(dest + 1, hunkbytes_ - 1) || !encoder_.EncodeInterleaved(src, srclen / 4, true))
				return 0;
			encoder_.Finish();
		}
		return complen + 1;
	}

private:
	static uint32_t BlockSize(uint32_t bytes) {
		// 16-65535, clamped to 2K samples, the sweet spot.
		uint32_t samples = bytes / 4;
		while (samples > 2048)
			samples /= 2;
		return samples;
	}

	uint32_t hunkbytes_;
	FlacEncoder encoder_;
};

// One thread's codecs, in the order of the header's compressor slots.
class CodecGroup {
public:
	CodecGroup(const uint32_t compression[4], uint32_t hunkbytes) : hunkbytes_(hunkbytes), test_(hunkbytes) {
		for (int i = 0; i < 4; i++) {
			switch (compression[i]) {
			case CODEC_ZLIB: codecs_[i].reset(new ZlibCodec()); break;
			case CODEC_ZSTD: codecs_[i].reset(new ZstdCodec()); break;
			case CODEC_LZMA: codecs_[i].reset(new LzmaCodec(hunkbytes)); break;
			case CODEC_HUFFMAN: codecs_[i].reset(new HuffmanCodec()); break;
			case CODEC_FLAC: codecs_[i].reset(new FlacCodec(hunkbytes)); break;
			default: break;
			}
		}
	}

	// The slot of the smallest result (strictly smaller than the hunk; earlier slots win ties), or
	// -1 to store the hunk as is.
	int FindBest(const uint8_t *src, uint8_t *compressed, uint32_t *complen) {
		*complen = hunkbytes_;
		int best = -1;
		for (int i = 0; i < 4; i++) {
			if (!codecs_[i])
				continue;
			const uint32_t size = codecs_[i]->Compress(src, hunkbytes_, test_.data());
			if (size != 0 && size < *complen) {
				best = i;
				*complen = size;
				memcpy(compressed, test_.data(), size);
			}
		}
		if (best == -1)
			memcpy(compressed, src, hunkbytes_);
		return best;
	}

private:
	uint32_t hunkbytes_;
	std::vector<uint8_t> test_;
	std::unique_ptr<Codec> codecs_[4];
};

struct HunkHash {
	uint16_t crc16;
	Sha1 sha1;
};

// chd_file_compressor's hashmap: the first hunk written with given contents.
class HunkMap {
public:
	static const uint64_t NOT_FOUND = ~0ULL;

	uint64_t Find(const HunkHash &hash) const {
		auto range = map_.equal_range(Key(hash));
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second.sha1 == hash.sha1)
				return it->second.hunk;
		}
		return NOT_FOUND;
	}
	void Add(uint64_t hunk, const HunkHash &hash) {
		map_.emplace(Key(hash), Entry{ hash.sha1, hunk });
	}

private:
	struct Entry {
		Sha1 sha1;
		uint64_t hunk;
	};
	static uint64_t Key(const HunkHash &hash) {
		uint64_t key = hash.crc16;
		for (int i = 0; i < 6; i++)
			key = (key << 8) | hash.sha1.raw[i];
		return key;
	}
	std::unordered_multimap<uint64_t, Entry> map_;
};

// chd.cpp's compress_v5_map.
bool CompressMap(const std::vector<uint8_t> &rawmap, uint32_t hunkcount, std::vector<uint8_t> *out) {
	const uint16_t mapcrc = Crc16(rawmap.data(), (size_t)hunkcount * 12);

	std::vector<uint8_t> compressionRle(hunkcount * 3 + 3);
	uint8_t *dest = compressionRle.data();

	// 16 different codes, at most 8 bits.
	HuffmanEncoder<16, 8> encoder;
	encoder.HistoReset();

	// RLE-compress the compression types, which come in runs.
	uint32_t maxSelf = 0;
	uint32_t lastSelf = 0;
	uint64_t maxParent = 0;
	uint32_t maxComplen = 0;
	uint8_t lastcomp = 0;
	int count = 0;
	for (uint32_t hunknum = 0; hunknum < hunkcount; hunknum++) {
		uint8_t curcomp = rawmap[hunknum * 12 + 0];
		if (curcomp == COMPRESSION_SELF) {
			// Promote self references to more compact forms.
			const uint32_t refhunk = (uint32_t)GetU48(&rawmap[hunknum * 12 + 4]);
			if (refhunk == lastSelf)
				curcomp = COMPRESSION_SELF_0;
			else if (refhunk == lastSelf + 1)
				curcomp = COMPRESSION_SELF_1;
			else
				maxSelf = std::max(maxSelf, refhunk);
			lastSelf = refhunk;
		} else {
			// No parents here. Like MAME, uncompressed hunks count towards the longest length too.
			maxComplen = std::max(maxComplen, GetU24(&rawmap[hunknum * 12 + 1]));
		}

		if (curcomp == lastcomp)
			count++;

		// No repeat, or at the end: flush it.
		if (curcomp != lastcomp || hunknum == hunkcount - 1) {
			while (count != 0) {
				if (count < 3) {
					encoder.HistoOne(*dest++ = lastcomp);
					count--;
				} else if (count <= 3 + 15) {
					encoder.HistoOne(*dest++ = COMPRESSION_RLE_SMALL);
					encoder.HistoOne(*dest++ = (uint8_t)(count - 3));
					count = 0;
				} else {
					const int thisCount = std::min(count, 3 + 16 + 255);
					encoder.HistoOne(*dest++ = COMPRESSION_RLE_LARGE);
					encoder.HistoOne(*dest++ = (uint8_t)((thisCount - 3 - 16) >> 4));
					encoder.HistoOne(*dest++ = (uint8_t)((thisCount - 3 - 16) & 15));
					count -= thisCount;
				}
			}
			if (curcomp != lastcomp)
				encoder.HistoOne(*dest++ = lastcomp = curcomp);
		}
	}

	const uint8_t lengthbits = BitWidth(maxComplen);
	const uint8_t selfbits = BitWidth(maxSelf);
	const uint8_t parentbits = BitWidth(maxParent);

	// 16 bytes of header, then at worst 12 bits per entry for the types and the widest extra data.
	const int nbitsNeeded = (8 * 16) + (12 + std::max<int>({ lengthbits + 16, selfbits, parentbits })) * (int)hunkcount;
	std::vector<uint8_t> compressed(nbitsNeeded / 8 + 1);
	BitstreamOut bitbuf(&compressed[16], (uint32_t)compressed.size() - 16);

	if (!encoder.ComputeTreeFromHisto() || !encoder.ExportTreeRle(bitbuf))
		return false;
	for (uint8_t *src = compressionRle.data(); src < dest; src++)
		encoder.EncodeOne(bitbuf, *src);

	// The data each entry type needs.
	lastcomp = 0;
	count = 0;
	uint8_t *src = compressionRle.data();
	uint64_t firstoffs = 0;
	for (uint32_t hunknum = 0; hunknum < hunkcount; hunknum++) {
		const uint8_t *entry = &rawmap[hunknum * 12];
		const uint32_t length = GetU24(&entry[1]);
		const uint64_t offset = GetU48(&entry[4]);
		const uint16_t crc = GetU16(&entry[10]);

		if (count == 0) {
			const uint8_t val = *src++;
			if (val == COMPRESSION_RLE_SMALL) {
				count = 2 + *src++;
			} else if (val == COMPRESSION_RLE_LARGE) {
				count = 2 + 16 + (*src++ << 4);
				count += *src++;
			} else {
				lastcomp = val;
			}
		} else {
			count--;
		}

		switch (lastcomp) {
		case COMPRESSION_TYPE_0:
		case COMPRESSION_TYPE_1:
		case COMPRESSION_TYPE_2:
		case COMPRESSION_TYPE_3:
			bitbuf.Write(length, lengthbits);
			bitbuf.Write(crc, 16);
			if (firstoffs == 0)
				firstoffs = offset;
			break;
		case COMPRESSION_NONE:
			bitbuf.Write(crc, 16);
			if (firstoffs == 0)
				firstoffs = offset;
			break;
		case COMPRESSION_SELF:
			bitbuf.Write((uint32_t)offset, selfbits);
			break;
		case COMPRESSION_PARENT:
			bitbuf.Write((uint32_t)offset, parentbits);
			break;
		default:
			break;
		}
	}

	const uint32_t complen = bitbuf.Flush();
	if (bitbuf.Overflow())
		return false;
	PutU32(&compressed[0], complen);
	PutU48(&compressed[4], firstoffs);
	PutU16(&compressed[10], mapcrc);
	compressed[12] = lengthbits;
	compressed[13] = selfbits;
	compressed[14] = parentbits;
	compressed[15] = 0;
	compressed.resize(complen + 16);
	*out = std::move(compressed);
	return true;
}

}  // namespace

bool Compress(const Path &input, const Path &output, Preset preset, const ImageProgressFunc &progress, std::string *error, bool *cancelled) {
	*cancelled = false;
	if (File::Exists(output)) {
		*error = "Output file already exists";
		return false;
	}
	DiscImageReader src;
	if (!src.Open(input, error)) {
		return false;
	}

	// CHDmodern's presets: chdman createdvd's codecs, or Zstandard alone.
	uint32_t compression[4] = { CODEC_LZMA, CODEC_ZLIB, CODEC_HUFFMAN, CODEC_FLAC };
	uint32_t hunkbytes = 2048;
	switch (preset) {
	case Preset::LargeHunk:
		hunkbytes = 32768;
		break;
	case Preset::QuickAccess:
		compression[0] = CODEC_ZSTD;
		compression[1] = compression[2] = compression[3] = 0;
		break;
	case Preset::SmallHunk:
	default:
		break;
	}

	const uint64_t logicalbytes = src.Size();
	const uint32_t hunkcount = (uint32_t)((logicalbytes + hunkbytes - 1) / hunkbytes);
	const uint32_t sectorsPerHunk = hunkbytes / UNIT_BYTES;
	const uint32_t numSectors = src.NumSectors();

	ImageOutputFile out;
	if (!out.Open(output, error)) {
		return false;
	}
	auto fail = [&](const char *message) {
		*error = message;
		return false;
	};

	// The header goes in last. Then the DVD metadata, an empty string: its 16-byte header and a 0.
	uint8_t header[V5_HEADER_SIZE]{};
	uint8_t meta[METADATA_HEADER_SIZE + 1]{};
	PutU32(&meta[0], DVD_METADATA_TAG);
	meta[4] = MDFLAGS_CHECKSUM;
	PutU24(&meta[5], 1);
	PutU64(&meta[8], 0);
	if (!out.Write(header, sizeof(header)) || !out.Write(meta, sizeof(meta))) {
		return fail("Could not write the output file");
	}
	const uint64_t metaoffset = V5_HEADER_SIZE;

	const unsigned threads = WorkerCount();
	const uint32_t batchHunks = std::max<uint32_t>(threads * 4, (uint32_t)(BATCH_BYTES / hunkbytes));
	std::vector<uint8_t> in((size_t)batchHunks * hunkbytes);
	std::vector<uint8_t> compressed((size_t)batchHunks * hunkbytes);
	std::vector<uint32_t> complen(batchHunks);
	std::vector<int> best(batchHunks);
	std::vector<HunkHash> hashes(batchHunks);
	std::vector<std::unique_ptr<CodecGroup>> groups(threads);
	std::vector<uint8_t> rawmap((size_t)hunkcount * 12);
	HunkMap current;
	Sha1Creator rawsha1;

	INFO_LOG(Log::Loader, "CHDCompress: %s -> %s, %u hunks of %u, %u threads", input.c_str(), output.c_str(), hunkcount, hunkbytes, threads);
	for (uint32_t first = 0; first < hunkcount; first += batchHunks) {
		if (progress && !progress((float)first / (float)hunkcount)) {
			*cancelled = true;
			return fail("Cancelled");
		}
		const uint32_t count = std::min(batchHunks, hunkcount - first);

		const uint32_t firstSector = first * sectorsPerHunk;
		const uint32_t sectors = std::min(count * sectorsPerHunk, numSectors - firstSector);
		if (!src.ReadSectors(firstSector, sectors, in.data())) {
			return fail("Could not read the game");
		}
		rawsha1.Append(in.data(), (size_t)sectors * UNIT_BYTES);
		if (sectors < count * sectorsPerHunk) {
			// The last hunk, past the end of the image. MAME reads into a ring of WORK_BUFFER_HUNKS
			// hunks and doesn't clear this part, so it holds the same part of the hunk that many
			// before (or zeros, from the start). Never read back, but it's compressed and stored, and
			// this keeps the output the same as chdman's.
			const uint32_t lastHunk = hunkcount - 1;
			const uint32_t valid = sectors - (count - 1) * sectorsPerHunk;
			uint8_t *pad = in.data() + (size_t)sectors * UNIT_BYTES;
			const size_t padBytes = (size_t)(sectorsPerHunk - valid) * UNIT_BYTES;
			if (lastHunk >= WORK_BUFFER_HUNKS) {
				if (!src.ReadSectors((lastHunk - WORK_BUFFER_HUNKS) * sectorsPerHunk + valid, sectorsPerHunk - valid, pad)) {
					return fail("Could not read the game");
				}
			} else {
				memset(pad, 0, padBytes);
			}
		}

		RunParallel(count, [&](unsigned worker, uint32_t i) {
			const uint8_t *data = in.data() + (size_t)i * hunkbytes;
			hashes[i].crc16 = Crc16(data, hunkbytes);
			hashes[i].sha1 = Sha1Creator::Simple(data, hunkbytes);
			best[i] = -2;
			// Hunks already written are copied by reference, no need to compress them (the map isn't
			// changed while this runs).
			if (current.Find(hashes[i]) != HunkMap::NOT_FOUND)
				return;
			if (!groups[worker])
				groups[worker].reset(new CodecGroup(compression, hunkbytes));
			best[i] = groups[worker]->FindBest(data, compressed.data() + (size_t)i * hunkbytes, &complen[i]);
		});

		// In order, like chd_file_compressor: a hunk seen before becomes a reference to it.
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t hunknum = first + i;
			uint8_t *entry = &rawmap[(size_t)hunknum * 12];
			const uint64_t selfhunk = current.Find(hashes[i]);
			if (selfhunk != HunkMap::NOT_FOUND) {
				entry[0] = COMPRESSION_SELF;
				PutU24(&entry[1], 0);
				PutU48(&entry[4], selfhunk);
				PutU16(&entry[10], 0);
				continue;
			}
			if (best[i] == -2) {
				// Only possible if the map changed under us.
				return fail("Internal error");
			}
			const uint64_t offset = out.Position();
			if (!out.Write(compressed.data() + (size_t)i * hunkbytes, complen[i])) {
				return fail("Could not write the output file");
			}
			entry[0] = best[i] == -1 ? COMPRESSION_NONE : (uint8_t)best[i];
			PutU24(&entry[1], complen[i]);
			PutU48(&entry[4], offset);
			PutU16(&entry[10], hashes[i].crc16);
			current.Add(hunknum, hashes[i]);
		}
	}

	std::vector<uint8_t> map;
	if (!CompressMap(rawmap, hunkcount, &map)) {
		return fail("Could not compress the hunk map");
	}
	const uint64_t mapoffset = out.Position();
	if (!out.Write(map.data(), map.size())) {
		return fail("Could not write the output file");
	}

	// The overall SHA-1: the raw data's, then each checksummed metadata entry's tag and SHA-1.
	const Sha1 raw = rawsha1.Finish();
	uint8_t metahash[24];
	PutU32(&metahash[0], DVD_METADATA_TAG);
	const uint8_t metadata = 0;
	const Sha1 metasha1 = Sha1Creator::Simple(&metadata, 1);
	memcpy(&metahash[4], metasha1.raw, 20);
	Sha1Creator overall;
	overall.Append(raw.raw, 20);
	overall.Append(metahash, sizeof(metahash));
	const Sha1 sha1 = overall.Finish();

	memcpy(&header[0], "MComprHD", 8);
	PutU32(&header[8], V5_HEADER_SIZE);
	PutU32(&header[12], 5);
	for (int i = 0; i < 4; i++)
		PutU32(&header[16 + i * 4], compression[i]);
	PutU64(&header[32], logicalbytes);
	PutU64(&header[40], mapoffset);
	PutU64(&header[48], metaoffset);
	PutU32(&header[56], hunkbytes);
	PutU32(&header[60], UNIT_BYTES);
	memcpy(&header[64], raw.raw, 20);
	memcpy(&header[84], sha1.raw, 20);
	// The parent's SHA-1 stays zero.
	if (!out.WriteAt(0, header, sizeof(header))) {
		return fail("Could not write the output file");
	}
	if (!out.Finish(error)) {
		return false;
	}
	if (progress) {
		progress(1.0f);
	}
	return true;
}

}  // namespace CHDCompress
