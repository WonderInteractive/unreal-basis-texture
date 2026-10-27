// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#pragma once

// The encoder core: Basis Universal encode plus the cook-time transcode, or a direct encode for
// the targets where that is better, with no Unreal types so that it builds and is tested on its
// own (Tests/). TextureFormatBasis.cpp is the Unreal side.

#include <cstdint>
#include <string>
#include <vector>

namespace BasisEncoder
{
	/**
	 * How the source's channels are arranged in the image handed to the encoder. The destination
	 * format decides this: two-channel targets (BC5, EAC RG11) take R and A, as Basis transcodes
	 * them from an intermediate, one-channel targets take R, and the normal-map layouts are Unreal's.
	 */
	enum class EChannelLayout : uint8_t
	{
		RGBA,  // unchanged; alpha forced opaque when bHasAlpha is false
		RRRG,  // R to RGB, G to A: BC5, EAC RG11, ASTC NormalLA
		RRR1,  // R to RGB, opaque: BC4, EAC R11
		RG01,  // (R, G, 0, 1): ASTC NormalRG
		AGR,   // (1, G, 0, R): DXT5n and ASTC NormalAG
	};

	enum class EOutput : uint8_t
	{
		/** Native blocks of the destination format: BC1-5 and EAC R11/RG11 encoded directly, the
		 *  rest transcoded at cook time from an intermediate (ASTC and BC7 losslessly). */
		Native,
		/** A supercompressed .basis payload the device transcodes at upload, when the destination
		 *  can be reached from one and the payload is no larger than the native blocks. Otherwise
		 *  Native, with FResult::Note saying why. Single-slice images only. */
		Payload,
	};

	/** Entropy coding of XUASTC payloads. Smaller payloads cost more CPU to transcode on the device. */
	enum class EPayloadCoding : uint8_t
	{
		Zstd,       // the library's default and the fastest to transcode
		Hybrid,     // about 5% smaller, 1.2x the transcode time
		Arithmetic, // about 10% smaller, 1.65x the transcode time
	};

	struct FSettings
	{
		/** Destination block format, as a basist::transcoder_texture_format. */
		int32_t TranscoderTarget = -1;
		EChannelLayout Layout = EChannelLayout::RGBA;
		bool bHasAlpha = true;
		/** Source and destination are sRGB-encoded colour (perceptual metrics, sRGB transfer flag). */
		bool bSRGB = false;
		EOutput Output = EOutput::Native;
		/** Payload quality, 1-100; 100 is lossless supercompression. Native output ignores it: the
		 *  intermediate is discarded, so trading its quality for its size buys nothing. */
		int32_t Quality = 100;
		/** Encoder effort 0-10, or -1 for each mode's library default (3). 1 costs XUASTC and ASTC
		 *  about 0.1 dB against 3 at a fifth of the time, and is as good or better for UASTC and XUBC7.
		 *  It also sets the EAC search radius (1 at effort 1 or less, 3 from effort 3). */
		int32_t Effort = 1;
		/** Payload intermediate block size for non-ASTC destinations (ASTC uses its own). */
		uint32_t PayloadBlockWidth = 6;
		uint32_t PayloadBlockHeight = 6;
		/** BC7 payloads use XUBC7 (supercompressed BC7) instead of XUASTC: larger, but higher
		 *  quality per byte at high Quality and a cheaper device transcode. */
		bool bBC7PayloadAsXUBC7 = false;
		/** XUASTC payload entropy coding; XUBC7 payloads ignore it. */
		EPayloadCoding PayloadCoding = EPayloadCoding::Zstd;
	};

	struct FResult
	{
		std::vector<uint8_t> Data;
		bool bPayload = false;
		/** Set when Encode returns false. */
		std::string Error;
		/** Set when a Payload request produced native blocks. */
		std::string Note;
	};

	/** One-time library initialisation. Thread-safe; Encode calls it. */
	void Init();

	/**
	 * Encode NumSlices tightly packed BGRA8 slices of Width x Height (slice after slice) to the
	 * destination format, which must be LDR. Native output is the slices' blocks concatenated,
	 * which is Unreal's mip layout for arrays, cubemaps and volumes.
	 */
	bool Encode(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out);

	/**
	 * Encode linear RGBA32F slices, laid out as for Encode, to an HDR destination: BC6H, ASTC HDR
	 * 4x4 or ASTC HDR 6x6. Only the target, output, quality and effort settings apply. Values are
	 * cleaned as for BC6H first: NaN, infinities and negatives become 0, values clamp to the
	 * largest ASTC HDR can hold (65216), and alpha is opaque.
	 */
	bool EncodeHDR(const float* RGBA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out);

	/**
	 * Decode what Encode wrote for an LDR target back to BGRA8 slices, as the GPU samples them:
	 * one- and two-channel formats come back as (R, 0, 0, 1) and (R, G, 0, 1). The ASTC normal-map
	 * layouts (RG01, RRRG, AGR) come back as (X, Y, 0, 1), as the engine's ASTC decoder returns
	 * them. A single-slice payload is transcoded first, as a device would. Size is the bytes at
	 * Data, at least the native size; BGRA receives Width * Height * NumSlices pixels.
	 */
	bool Decode(const uint8_t* Data, uint64_t Size, int32_t TranscoderTarget, uint32_t Width, uint32_t Height, uint32_t NumSlices,
		EChannelLayout Layout, bool bSRGB, uint8_t* BGRA);

	/** Decode an HDR target's blocks (or payload) to linear RGBA32F slices; alpha is 1. */
	bool DecodeHDR(const uint8_t* Data, uint64_t Size, int32_t TranscoderTarget, uint32_t Width, uint32_t Height, uint32_t NumSlices, float* RGBA);

	/** Whether the target is one of the HDR block formats EncodeHDR writes. */
	bool IsHDRTarget(int32_t TranscoderTarget);

	/** Bytes of native blocks for one Width x Height slice of the target, or 0 for a target that is not a block format. */
	uint64_t NativeSliceSize(int32_t TranscoderTarget, uint32_t Width, uint32_t Height);

	/** The basist::basis_tex_format Encode uses as its intermediate for these settings, or -1 when
	 *  there is none: native BC1-5 and EAC encode directly, and an invalid target has none. */
	int32_t IntermediateFormat(const FSettings& Settings);

	/** Whether these settings would request a payload at all (Encode can still fall back to native per image). */
	bool WantsPayload(const FSettings& Settings);

	/** Library name for a basis_tex_format or transcoder_texture_format value, for logs. */
	const char* IntermediateFormatName(int32_t BasisTexFormat);
	const char* TargetFormatName(int32_t TranscoderTarget);

	/** The vendored basis_universal version, e.g. "2.50". */
	const char* LibraryVersion();
}
