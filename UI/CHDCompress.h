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

#include <string>

#include "Common/File/Path.h"
#include "UI/DiscImage.h"

// Writes PSP disc images as DVD CHDs (CHD v5, 2048-byte units), like "chdman createdvd". The
// input is anything PPSSPP can play as a disc (UI/DiscImage.h). Reading them back is libchdr's job.
// The output matches CHDmodern's (LunaMoo/CHDmodern, MAME's chd.cpp) byte for byte.
namespace CHDCompress {

enum class Preset {
	LargeHunk = 0,    // chdman's codecs (LZMA, zlib, Huffman, FLAC), 32K hunks. Smallest for most games.
	SmallHunk = 1,    // The same codecs, 2K hunks. Smallest when a game repeats a lot of data.
	QuickAccess = 2,  // Zstandard, 2K hunks. Bigger, but fastest to read while playing.
};

// Blocking, and uses every core. Never overwrites, and deletes a partial output on failure or
// cancel (*cancelled is set then).
bool Compress(const Path &input, const Path &output, Preset preset, const ImageProgressFunc &progress, std::string *error, bool *cancelled);

}  // namespace CHDCompress
