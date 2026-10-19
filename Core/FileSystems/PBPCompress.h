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
#include "Core/FileSystems/DiscImageIO.h"

// Writes PSP disc images as PSN-style EBOOT.PBPs (NPUMDIMG) through iPBP (ext/ipbp). The input is
// anything PPSSPP can play as a disc (DiscImageIO.h). Reading them back is PPSSPP's
// NPDRMDemoBlockDevice's job.
namespace PBPCompress {

enum class Preset {
	Max = 0,            // The smallest LZRC (optimal parsing). Still LZRC, but not sign_np's output.
	PSPCompatible = 1,  // LZRC like sign_np: what real PSPs are known to read.
	Uncompressed = 2,   // Blocks stored as they are.
};

// Blocking, and uses every core. Never overwrites, and deletes a partial output on failure or
// cancel (*cancelled is set then).
bool Compress(const Path &input, const Path &output, Preset preset, const ImageProgressFunc &progress, std::string *error, bool *cancelled);

}  // namespace PBPCompress
