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
	 * destination format. Native output is the slices' blocks concatenated, which is Unreal's
	 * mip layout for arrays, cubemaps and volumes.
	 */
	bool Encode(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out);

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
