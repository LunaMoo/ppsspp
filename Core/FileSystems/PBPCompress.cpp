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
#include <cstring>
#include <vector>

#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/FileSystems/PBPCompress.h"

#include "ext/ipbp/include/ipbp/ipbp.hpp"
#include "ext/ipbp/src/util.h"

namespace PBPCompress {

namespace {

const uint32_t SECTOR_SIZE = DiscImageReader::SECTOR_SIZE;

// iPBP reads at any byte offset (the ISO's file system too), the block device in sectors.
class DiscInput : public ipbp::InputStream {
public:
	explicit DiscInput(DiscImageReader &src) : src_(src) {}

	uint64_t size() const override { return src_.Size(); }

	bool readAt(uint64_t offset, void *buf, size_t len) override {
		if (offset > src_.Size() || len > src_.Size() - offset) {
			return false;
		}
		uint8_t *out = (uint8_t *)buf;
		while (len > 0) {
			const uint32_t sector = (uint32_t)(offset / SECTOR_SIZE);
			const uint32_t inSector = (uint32_t)(offset % SECTOR_SIZE);
			if (inSector == 0 && len >= SECTOR_SIZE) {
				// Whole sectors straight into the caller's buffer.
				const uint32_t count = (uint32_t)(len / SECTOR_SIZE);
				if (!src_.ReadSectors(sector, count, out)) {
					return false;
				}
				const size_t bytes = (size_t)count * SECTOR_SIZE;
				out += bytes;
				offset += bytes;
				len -= bytes;
				continue;
			}
			if (sector != cached_) {
				if (!src_.ReadSectors(sector, 1, cache_)) {
					return false;
				}
				cached_ = sector;
			}
			const size_t n = std::min<size_t>(len, SECTOR_SIZE - inSector);
			memcpy(out, cache_ + inSector, n);
			out += n;
			offset += n;
			len -= n;
		}
		return true;
	}

private:
	DiscImageReader &src_;
	uint8_t cache_[SECTOR_SIZE];
	uint32_t cached_ = 0xFFFFFFFF;
};

class FileOutput : public ipbp::OutputStream {
public:
	explicit FileOutput(ImageOutputFile &out) : out_(out) {}

	bool writeAt(uint64_t offset, const void *buf, size_t len) override {
		return out_.WriteAt(offset, buf, len);
	}
	bool finish(uint64_t finalSize) override {
		// iPBP never writes past what it ends up with, so there's nothing to cut off.
		return out_.Position() == finalSize;
	}

private:
	ImageOutputFile &out_;
};

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
	ImageOutputFile out;
	if (!out.Open(output, error)) {
		return false;
	}

	ipbp::IsoToPbpOptions options;
	options.compress = preset != Preset::Uncompressed;
	options.maxCompression = preset == Preset::Max;

	ipbp::Callbacks callbacks;
	callbacks.progress = [&](uint64_t done, uint64_t total) {
		if (progress && total > 0 && !progress((float)((double)done / (double)total))) {
			callbacks.cancel.cancel();
		}
	};
	callbacks.log = [](const std::string &line) {
		INFO_LOG(Log::Loader, "iPBP: %s", line.c_str());
	};

	DiscInput in(src);
	FileOutput pbp(out);
	INFO_LOG(Log::Loader, "PBPCompress: %s -> %s", input.c_str(), output.c_str());
	const ipbp::Result result = ipbp::isoToPbp(in, pbp, options, callbacks);
	if (!result.ok()) {
		*cancelled = result.status == ipbp::Status::Cancelled;
		*error = result.message.empty() ? std::string(ipbp::statusName(result.status)) : result.message;
		return false;
	}
	if (!out.Finish(error)) {
		return false;
	}
	if (progress) {
		progress(1.0f);
	}
	return true;
}

}  // namespace PBPCompress

// iPBP's file I/O (its io.cpp), on PPSSPP's file layer so it works everywhere, content URIs and
// UWP included. Only its path-based conversions use these; PPSSPP uses the stream ones.
namespace ipbp {

namespace {

class CFileInput : public InputStream {
public:
	CFileInput(FILE *f, uint64_t size) : f_(f), size_(size) {}
	~CFileInput() override { fclose(f_); }
	uint64_t size() const override { return size_; }
	bool readAt(uint64_t offset, void *buf, size_t len) override {
		if (offset + len > size_ || File::Fseek(f_, (int64_t)offset, SEEK_SET) != 0) {
			return false;
		}
		return fread(buf, 1, len, f_) == len;
	}

private:
	FILE *f_;
	uint64_t size_;
};

class CFileOutput : public OutputStream {
public:
	explicit CFileOutput(FILE *f) : f_(f) {}
	~CFileOutput() override {
		if (f_) {
			fclose(f_);
		}
	}
	bool writeAt(uint64_t offset, const void *buf, size_t len) override {
		if (File::Fseek(f_, (int64_t)offset, SEEK_SET) != 0) {
			return false;
		}
		const bool ok = fwrite(buf, 1, len, f_) == len;
		end_ = std::max(end_, offset + len);
		return ok;
	}
	bool finish(uint64_t finalSize) override {
		// Nothing past finalSize gets written, so no truncating.
		const bool ok = end_ <= finalSize && fclose(f_) == 0;
		f_ = nullptr;
		return ok;
	}

private:
	FILE *f_;
	uint64_t end_ = 0;
};

}  // namespace

std::unique_ptr<InputStream> openInputFile(const std::string &utf8Path, std::string *error) {
	const Path path(utf8Path);
	FILE *f = File::OpenCFile(path, "rb");
	if (!f) {
		if (error) *error = "cannot open '" + utf8Path + "'";
		return nullptr;
	}
	return std::unique_ptr<InputStream>(new CFileInput(f, File::GetFileSize(path)));
}

std::unique_ptr<OutputStream> openOutputFile(const std::string &utf8Path, std::string *error) {
	const Path path(utf8Path);
	FILE *f = File::OpenCFile(path, "wb");
	if (!f) {
		if (error) *error = "cannot create '" + utf8Path + "'";
		return nullptr;
	}
	return std::unique_ptr<OutputStream>(new CFileOutput(f));
}

namespace detail {

bool removeFile(const std::string &path) {
	return File::Delete(Path(path), true);
}

bool renameReplace(const std::string &from, const std::string &to) {
	if (File::Exists(Path(to))) {
		File::Delete(Path(to), true);
	}
	return File::Rename(Path(from), Path(to));
}

}  // namespace detail

}  // namespace ipbp
