#if defined(__i386__) || defined(_M_IX86)
#define FLAC__CPU_IA32
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define FLAC__CPU_X86_64
#endif

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define FLAC__ALIGN_MALLOC_DATA
#define FLAC__HAS_X86INTRIN 1
#else
#define FLAC__HAS_X86INTRIN 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define FLAC__CPU_ARM64
#endif

/* lpc_intrin_neon.c initialises NEON vectors GCC/Clang-style, which MSVC
   rejects (C2078); MSVC ARM64 builds use the portable C code instead */
#if (defined(__aarch64__) || defined(_M_ARM64)) && !(defined(_MSC_VER) && !defined(__clang__))
#define FLAC__HAS_NEONINTRIN 1
#define FLAC__HAS_A64NEONINTRIN 1
#else
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_A64NEONINTRIN 0
#endif

#define PACKAGE_VERSION "1.4.3"
