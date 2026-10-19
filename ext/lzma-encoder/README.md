LZMA encoder from the [LZMA SDK](https://www.7-zip.org/sdk.html) 24.05, as libchdr shipped it before
it moved to 26.02 (which only has the decoder). Used by the CHD writer, `Core/FileSystems/CHDCompress.cpp`.

Unmodified copies of `LzmaEnc.c`, `LzFind.c`, `CpuArch.c` and the headers they include.
`lzma_encoder.c` is the only file to compile. The LZMA SDK is in the public domain, see LICENSE.
