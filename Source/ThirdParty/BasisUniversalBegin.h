// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

// Include before any basis_universal header or source, and BasisUniversalEnd.h after. Every copy
// of the library the plugin compiles (both modules and the tests) is configured here, and compiler
// warnings are silenced in between: upstream is built with its own warning hygiene, not the
// engine's warnings-as-errors set, while the plugin's own code keeps the engine's. No include
// guard; each Begin pairs with an End. C and C++.

// No Unreal pixel format maps to these transcoder targets, so their decoders are dead weight.
#define BASISD_SUPPORT_PVRTC1 0
#define BASISD_SUPPORT_PVRTC2 0
#define BASISD_SUPPORT_ATC 0
#define BASISD_SUPPORT_FXT1 0

// Payloads are .basis files, never KTX2 containers, but the encoder requires the KTX2 code, so
// only a transcoder no encoder shares leaves it out (BasisTranscoder.Build.cs decides). zstd is
// unaffected: BASISD_SUPPORT_KTX2_ZSTD gates it, and XUASTC and XUBC7 need it.
#if defined(BASIS_UE_TRANSCODER_ONLY) && BASIS_UE_TRANSCODER_ONLY
	#define BASISD_SUPPORT_KTX2 0
#endif

// Encoder: no OpenCL, and no second ASTC decoder (Android's, which upstream uses only to
// cross-check its own).
#define BASISU_SUPPORT_OPENCL 0
#define BASISU_DISABLE_ANDROID_ASTC_DECOMP 1

// The encoder's SSE4.1 kernels on x64. Unreal's x64 minimum is SSE4.2 (clang builds pass
// -msse4.2, and MSVC needs no flag for the intrinsics), and upstream checks the CPU before using
// them. Decided here rather than in a Build.cs, whose rules can cover several architectures at
// once (universal Mac binaries).
#ifndef BASISU_SUPPORT_SSE
	#if defined(__x86_64__) || defined(_M_X64)
		#define BASISU_SUPPORT_SSE 1
	#else
		#define BASISU_SUPPORT_SSE 0
	#endif
#endif

#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
	#pragma warning(push, 0)
	// Unreal promotes these with /we (shadowing, undefined macro in #if, unreachable and
	// possibly uninitialised code); disable them by number as well as by level.
	#pragma warning(disable: 4456 4457 4458 4459 4668 4701 4702 4703)
#elif defined(__GNUC__)
	// GCC has no -Weverything and ignores group options such as -Wall here, so each warning the
	// library trips is listed (as built by Tests/ with GCC's strict set).
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wshadow"
	#pragma GCC diagnostic ignored "-Wundef"
	#pragma GCC diagnostic ignored "-Wunused-variable"
	#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
	#pragma GCC diagnostic ignored "-Wunused-function"
	#pragma GCC diagnostic ignored "-Wstringop-overflow"
	#pragma GCC diagnostic ignored "-Wstrict-aliasing"
	#pragma GCC diagnostic ignored "-Wmisleading-indentation"
	#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
