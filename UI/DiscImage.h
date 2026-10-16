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

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "Common/File/Path.h"

class BlockDevice;
class FileLoader;

// Converting PSP disc images, natively: CHD, CSO and PBP writers (UI/CHDCompress.h,
// UI/CSOCompress.h, UI/PBPCompress.h), and plain ISO out of anything PPSSPP plays.

// Return false to cancel.
typedef std::function<bool(float progress)> ImageProgressFunc;

// Anything PPSSPP plays as a disc (ISO, CSO, CHD, a PSN EBOOT.PBP), as 2048-byte sectors, through
// PPSSPP's own block devices. Not thread-safe.
class DiscImageReader {
public:
	DiscImageReader();
	~DiscImageReader();

	bool Open(const Path &path, std::string *error);
	uint64_t Size() const { return (uint64_t)numSectors_ * SECTOR_SIZE; }
	uint32_t NumSectors() const { return numSectors_; }
	bool ReadSectors(uint32_t first, uint32_t count, uint8_t *out);

	static const uint32_t SECTOR_SIZE = 2048;

private:
	std::unique_ptr<FileLoader> loader_;
	std::unique_ptr<BlockDevice> device_;
	uint32_t numSectors_ = 0;
};

// A new output file, deleted again unless Finish() succeeds.
class ImageOutputFile {
public:
	~ImageOutputFile();

	// Refuses to overwrite.
	bool Open(const Path &path, std::string *error);
	bool Write(const void *data, size_t size);
	// Writes anywhere: earlier (headers written last) or past the end, which then moves.
	bool WriteAt(uint64_t offset, const void *data, size_t size);
	// The end of the file, where Write() goes.
	uint64_t Position() const { return pos_; }
	bool Finish(std::string *error);
	void Discard();

private:
	Path path_;
	FILE *f_ = nullptr;
	uint64_t pos_ = 0;
};

// Values are stored in the config (iUnPackFormat) and index the menus.
enum class ImageFormat {
	CHD = 0,
	CSO = 1,
	PBP = 2,
};

// The extension written for a format, with the dot.
const char *ImageFormatExtension(ImageFormat format);

// Blocking: compresses any disc image PPSSPP plays into the format, the preset indexed like
// iCHDPreset, iCSOPreset and iPBPPreset. Never overwrites, and deletes a partial output on failure
// or cancel (*cancelled is set then).
bool CompressDiscImage(ImageFormat format, int preset, const Path &input, const Path &output, const ImageProgressFunc &progress, std::string *error, bool *cancelled);
// The same, writing a plain ISO.
bool ExtractDiscImage(const Path &input, const Path &output, const ImageProgressFunc &progress, std::string *error, bool *cancelled);

// For a .pbp: whether it holds a PSP UMD image (homebrew and PS1 EBOOTs don't), and its disc id
// ("ULUS-10041"), maybe empty.
bool ReadUmdPbpInfo(const Path &pbp, std::string *discId);

// Calls work(worker, index) for every index below count, on as many threads as there are cores,
// and returns when all are done. worker is below WorkerCount(), for per-thread state.
unsigned WorkerCount();
void RunParallel(uint32_t count, const std::function<void(unsigned worker, uint32_t index)> &work);
