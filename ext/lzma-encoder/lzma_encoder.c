// The LZMA encoder from the LZMA SDK 24.05 (public domain, see LICENSE), for the CHD writer
// (Core/FileSystems/CHDCompress.cpp). libchdr only has the decoder since it moved to 26.02.
// One file to compile, so each build system needs just this one, and the files keep their
// relative includes. Single threaded, so LzFindMt isn't needed.

#define Z7_ST

#include "CpuArch.c"
#include "LzFind.c"
#include "LzmaEnc.c"
