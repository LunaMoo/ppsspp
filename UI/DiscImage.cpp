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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "Common/File/FileUtil.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/Loaders.h"
#include "UI/CHDCompress.h"
#include "UI/CSOCompress.h"
#include "UI/DiscImage.h"
#include "UI/PBPCompress.h"

DiscImageReader::DiscImageReader() {}
DiscImageReader::~DiscImageReader() {
	// The device refers to the loader.
	device_.reset();
	loader_.reset();
}

bool DiscImageReader::Open(const Path &path, std::string *error) {
	loader_.reset(ConstructFileLoader(path));
	if (!loader_ || !loader_->Exists()) {
		*error = "Could not open the game";
		return false;
	}
	char magic[4]{};
	if (loader_->ReadAt(0, 4, magic) == 4 && !memcmp(magic, "\0PBP", 4) && !ReadUmdPbpInfo(path, nullptr)) {
		// The block devices would take it as a raw image.
		*error = "Not a PSP game EBOOT (homebrew or PS1?)";
		return false;
	}
	device_.reset(ConstructBlockDevice(loader_.get(), error));
	if (!device_) {
		return false;
	}
	numSectors_ = device_->GetNumBlocks();
	if (device_->GetUncompressedSize() != Size()) {
		*error = "The image isn't a whole number of sectors";
		return false;
	}
	if (numSectors_ == 0) {
		*error = "The image is empty";
		return false;
	}
	return true;
}

bool DiscImageReader::ReadSectors(uint32_t first, uint32_t count, uint8_t *out) {
	if (count == 0) {
		return true;
	}
	if (first > numSectors_ || count > numSectors_ - first) {
		return false;
	}
	return device_->ReadBlocks(first, (int)count, out);
}

ImageOutputFile::~ImageOutputFile() {
	Discard();
}

bool ImageOutputFile::Open(const Path &path, std::string *error) {
	if (File::Exists(path)) {
		*error = "Output file already exists";
		return false;
	}
	f_ = File::OpenCFile(path, "wb");
	if (!f_) {
		*error = "Could not create the output file";
		return false;
	}
	path_ = path;
	pos_ = 0;
	return true;
}

bool ImageOutputFile::Write(const void *data, size_t size) {
	if (!f_) {
		return false;
	}
	if (size != 0 && fwrite(data, 1, size, f_) != size) {
		return false;
	}
	pos_ += size;
	return true;
}

bool ImageOutputFile::WriteAt(uint64_t offset, const void *data, size_t size) {
	if (!f_) {
		return false;
	}
	if (offset == pos_) {
		return Write(data, size);
	}
	// Past the end, the gap reads back as zeros.
	bool ok = File::Fseek(f_, (int64_t)offset, SEEK_SET) == 0 && (size == 0 || fwrite(data, 1, size, f_) == size);
	if (ok && offset + size > pos_) {
		pos_ = offset + size;
		return true;
	}
	return File::Fseek(f_, (int64_t)pos_, SEEK_SET) == 0 && ok;
}

bool ImageOutputFile::Finish(std::string *error) {
	if (!f_) {
		return false;
	}
	FILE *f = f_;
	f_ = nullptr;
	if (fclose(f) != 0) {
		File::Delete(path_, true);
		*error = "Could not write the output file";
		return false;
	}
	path_ = Path();
	return true;
}

void ImageOutputFile::Discard() {
	if (f_) {
		fclose(f_);
		f_ = nullptr;
		File::Delete(path_, true);
	}
	path_ = Path();
}

const char *ImageFormatExtension(ImageFormat format) {
	switch (format) {
	case ImageFormat::CHD: return ".chd";
	case ImageFormat::PBP: return ".pbp";
	default: return ".cso";
	}
}

bool CompressDiscImage(ImageFormat format, int preset, const Path &input, const Path &output, const ImageProgressFunc &progress, std::string *error, bool *cancelled) {
	preset = std::clamp(preset, 0, 2);
	switch (format) {
	case ImageFormat::CHD: return CHDCompress::Compress(input, output, (CHDCompress::Preset)preset, progress, error, cancelled);
	case ImageFormat::PBP: return PBPCompress::Compress(input, output, (PBPCompress::Preset)preset, progress, error, cancelled);
	default: return CSOCompress::Compress(input, output, (CSOCompress::Preset)preset, progress, error, cancelled);
	}
}

bool ExtractDiscImage(const Path &input, const Path &output, const ImageProgressFunc &progress, std::string *error, bool *cancelled) {
	*cancelled = false;
	if (File::Exists(output)) {
		*error = "Output file already exists";
		return false;
	}
	DiscImageReader src;
	if (!src.Open(input, error)) {
		return false;
	}
	ImageOutputFile out;
	if (!out.Open(output, error)) {
		return false;
	}

	const uint32_t numSectors = src.NumSectors();
	const uint32_t batch = 16384;  // 32 MB
	std::vector<uint8_t> buffer((size_t)batch * DiscImageReader::SECTOR_SIZE);
	for (uint32_t first = 0; first < numSectors; first += batch) {
		if (progress && !progress((float)first / (float)numSectors)) {
			*cancelled = true;
			*error = "Cancelled";
			return false;
		}
		const uint32_t count = std::min(batch, numSectors - first);
		if (!src.ReadSectors(first, count, buffer.data())) {
			*error = "Could not read the game";
			return false;
		}
		if (!out.Write(buffer.data(), (size_t)count * DiscImageReader::SECTOR_SIZE)) {
			*error = "Could not write the output file";
			return false;
		}
	}
	if (!out.Finish(error)) {
		return false;
	}
	if (progress) {
		progress(1.0f);
	}
	return true;
}

bool ReadUmdPbpInfo(const Path &pbp, std::string *discId) {
	std::unique_ptr<FileLoader> loader(ConstructFileLoader(pbp));
	if (!loader || !loader->Exists()) {
		return false;
	}
	// The PBP header's DATA.PSAR offset, and there an NPUMDIMG header with the disc id at 0x70.
	uint8_t header[0x28]{};
	if (loader->ReadAt(0, sizeof(header), header) != sizeof(header) || memcmp(header, "\0PBP", 4) != 0) {
		return false;
	}
	const uint32_t psar = header[0x24] | (header[0x25] << 8) | (header[0x26] << 16) | ((uint32_t)header[0x27] << 24);
	uint8_t np[0x80]{};
	if (loader->ReadAt(psar, sizeof(np), np) != sizeof(np) || memcmp(np, "NPUMDIMG", 8) != 0) {
		return false;
	}
	if (discId) {
		discId->assign((const char *)np + 0x70, strnlen((const char *)np + 0x70, 10));
	}
	return true;
}

unsigned WorkerCount() {
	return std::max(1u, std::thread::hardware_concurrency());
}

void RunParallel(uint32_t count, const std::function<void(unsigned worker, uint32_t index)> &work) {
	std::atomic<uint32_t> next{0};
	auto loop = [&](unsigned worker) {
		uint32_t i;
		while ((i = next++) < count) {
			work(worker, i);
		}
	};
	const unsigned workers = std::min<unsigned>(WorkerCount(), std::max<uint32_t>(count, 1));
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < workers; ++t) {
		pool.emplace_back([&loop, t]() {
			SetCurrentThreadName("ImageCompress");
			loop(t);
		});
	}
	loop(0);
	for (std::thread &t : pool) {
		t.join();
	}
}
