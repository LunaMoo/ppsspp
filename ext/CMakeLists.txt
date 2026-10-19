set(ARMIPS_REGEXP OFF CACHE BOOL "" FORCE)
set(ARMIPS_LIBRARY_ONLY ON CACHE BOOL "" FORCE)
if(WIN32)
	set(ARMIPS_USE_STD_FILESYSTEM ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(armips)

if(NOT USING_GLES2 AND NOT NO_OPENGL)
	add_subdirectory(glew)
endif()

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "let's not build binaries we don't need" FORCE)
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "let's not use exceptions" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "we don't need spvremapper" FORCE)

# not really needed since we currently bypass the official cmakelists...
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "we don't need zlib examples" FORCE)

# We only feed glslang GLSL, so its HLSL frontend is dead weight (and once hit an NDK 20 compiler
# issue, PPSSPP issue #12105). Windows keeps it, to match the MSVC and UWP projects.
if(NOT WIN32)
	set(ENABLE_HLSL OFF CACHE BOOL "let's not build HLSL support we don't need" FORCE)
endif()

# This works around a bug in cpu_features when ${CMAKE_BINARY_DIR} != ${CMAKE_CURRENT_BINRARY_DIR}.
# We also disable the list_cpu_features binary.
add_subdirectory(cmake/cpu_features EXCLUDE_FROM_ALL)

set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_INSTALL OFF)
add_subdirectory(glslang EXCLUDE_FROM_ALL)
add_subdirectory(snappy)
add_subdirectory(lua-build)
add_subdirectory(at3_standalone)
add_subdirectory(udis86)
add_subdirectory(SPIRV-Cross-build)
add_subdirectory(rcheevos-build)
add_subdirectory(gason)
add_subdirectory(vma)
add_subdirectory(cityhash)
add_subdirectory(lzma-sdk)
add_subdirectory(basis_universal)
add_subdirectory(pugixml)
add_subdirectory(libkirk)
add_subdirectory(sfmt19937)
add_subdirectory(xbrz)

add_library(xxhash STATIC
	xxhash.c
	xxhash.h
)

# Only the deflate encoder, for writing CSO files (Core/FileSystems/CSOCompress.cpp).
add_library(libdeflate STATIC
	libdeflate/lib/deflate_compress.c
	libdeflate/lib/utils.c
)

# libFLAC, for CHD's FLAC codec (Core/FileSystems/CHDCompress.cpp). Configured like CHDmodern builds it.
add_library(flac STATIC
	flac/src/libFLAC/bitmath.c
	flac/src/libFLAC/bitreader.c
	flac/src/libFLAC/bitwriter.c
	flac/src/libFLAC/cpu.c
	flac/src/libFLAC/crc.c
	flac/src/libFLAC/fixed.c
	flac/src/libFLAC/fixed_intrin_avx2.c
	flac/src/libFLAC/fixed_intrin_sse2.c
	flac/src/libFLAC/fixed_intrin_sse42.c
	flac/src/libFLAC/fixed_intrin_ssse3.c
	flac/src/libFLAC/float.c
	flac/src/libFLAC/format.c
	flac/src/libFLAC/lpc.c
	flac/src/libFLAC/lpc_intrin_avx2.c
	flac/src/libFLAC/lpc_intrin_fma.c
	flac/src/libFLAC/lpc_intrin_neon.c
	flac/src/libFLAC/lpc_intrin_sse2.c
	flac/src/libFLAC/lpc_intrin_sse41.c
	flac/src/libFLAC/md5.c
	flac/src/libFLAC/memory.c
	flac/src/libFLAC/stream_decoder.c
	flac/src/libFLAC/stream_encoder.c
	flac/src/libFLAC/stream_encoder_framing.c
	flac/src/libFLAC/stream_encoder_intrin_avx2.c
	flac/src/libFLAC/stream_encoder_intrin_sse2.c
	flac/src/libFLAC/stream_encoder_intrin_ssse3.c
	flac/src/libFLAC/window.c
)
if(WIN32)
	target_sources(flac PRIVATE flac/src/share/win_utf8_io/win_utf8_io.c)
endif()
target_include_directories(flac PUBLIC flac/include PRIVATE flac/src/libFLAC/include)
target_compile_definitions(flac
	PUBLIC FLAC__NO_DLL
	PRIVATE
		NDEBUG HAVE_CONFIG_H OGG_FOUND=0 FLAC__HAS_OGG=0 HAVE_LROUND=1
		HAVE_INTTYPES_H HAVE_STDBOOL_H HAVE_STDINT_H HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
		CPU_IS_BIG_ENDIAN=0 CPU_IS_LITTLE_ENDIAN=1 WORDS_BIGENDIAN=0
		# Before API 24, bionic hides fseeko/ftello with _FILE_OFFSET_BITS=64 on 32-bit ABIs. Only
		# the stream API is used, never FLAC's file I/O.
		$<$<NOT:$<OR:$<C_COMPILER_ID:MSVC>,$<PLATFORM_ID:Android>>>:HAVE_FSEEKO _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE>
		$<$<PLATFORM_ID:Darwin,iOS>:FLAC__SYS_DARWIN>
		$<$<PLATFORM_ID:Linux,Android>:FLAC__SYS_LINUX>)
if(NOT MSVC)
	target_link_libraries(flac m)
endif()

# iPBP's conversion core, for the PBP presets (Core/FileSystems/PBPCompress.cpp).
add_library(ipbp STATIC
	ipbp/src/ipbp_util.cpp
	ipbp/src/convert.cpp
	ipbp/src/crypto/aes.cpp
	ipbp/src/crypto/ecdsa.cpp
	ipbp/src/crypto/sha1.cpp
	ipbp/src/psp/amctrl.cpp
	ipbp/src/psp/iso9660.cpp
	ipbp/src/psp/lzrc.cpp
	ipbp/src/psp/sfo.cpp
)
if(WIN32)
	target_compile_definitions(ipbp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(aemu_postoffice_client_obj
	aemu_postoffice/client/postoffice.c
	aemu_postoffice/client/postoffice_mem_stdc.c
	aemu_postoffice/client/mutex_impl_cpp.cpp
	aemu_postoffice/client/delay_impl_cpp.cpp
	aemu_postoffice/client/log_impl_ppsspp.cpp
)

if(WIN32)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice/client/sock_impl_windows.c
	)
endif()

if(LINUX OR ANDROID OR MACOSX OR IOS OR BSD)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice/client/sock_impl_linux.c
	)
endif()

add_library(aemu_postoffice_client STATIC
	${aemu_postoffice_client_obj}
)

set(FT_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BZIP2 OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_PNG OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
# Only AtlasTool uses FreeType.
if(ATLAS_TOOL AND NOT USE_SYSTEM_FREETYPE)
    add_subdirectory(freetype freetype-build)
endif()

if(NOT LIBRETRO)
    add_subdirectory(imgui)
endif()
if(NOT HTTPS_NOT_AVAILABLE)
	add_subdirectory(naett-build)
endif()
if(USE_DISCORD AND NOT IOS AND NOT LIBRETRO)
	add_subdirectory(discord-rpc-build)
endif()

if(NOT USE_SYSTEM_LIBCHDR)
	add_subdirectory(libchdr-build)
else()
	# The CHD writer's LZMA encoder (Core/FileSystems/CHDCompress.cpp), which the bundled libchdr has otherwise.
	add_library(lzma_encoder STATIC
		libchdr/deps/lzma-24.05/src/CpuArch.c
		libchdr/deps/lzma-24.05/src/LzFind.c
		libchdr/deps/lzma-24.05/src/LzmaEnc.c
	)
	target_include_directories(lzma_encoder PRIVATE libchdr/deps/lzma-24.05/include)
	target_compile_definitions(lzma_encoder PRIVATE Z7_ST)
endif()

if(BUILD_BUNDLED_FFMPEG)
	add_subdirectory(ffmpeg-build)
endif()

if(ANDROID)
    if (ARM64)
        add_subdirectory(libadrenotools)
    endif()
endif()
