// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#pragma once

// The runtime transcode core, free of Unreal types so that it builds and is tested on its own
// (Tests/). BasisTranscoder.cpp wraps it for Unreal.

#include <cstdint>

namespace BasisTranscodeCore
{
	struct FPayloadInfo
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		/** Bytes the payload occupies; the buffer holding it may be longer. */
		uint32_t PayloadSize = 0;
		/** The intermediate, as a basist::basis_tex_format. */
		int32_t SourceFormat = -1;
		bool bHasAlpha = false;
	};

	/** One-time transcoder table setup. Thread-safe; every other function calls it. */
	void Init();

	/**
	 * Whether Data starts with a payload this plugin emits: a .basis file with one image of one
	 * level, whose header checksum holds. Cheap, and safe on arbitrary bytes. Size may exceed
	 * the payload (it is usually the native mip buffer it was copied into).
	 */
	bool GetPayloadInfo(const void* Data, uint64_t Size, FPayloadInfo& Out);

	/** Whether the payload's intermediate can produce TranscoderTarget (a basist::transcoder_texture_format). */
	bool CanTranscode(const void* Data, uint64_t Size, int32_t TranscoderTarget);

	/** Native bytes for a Width x Height image of the target, or 0 if it is not a block format. */
	uint64_t TranscodedSize(int32_t TranscoderTarget, uint32_t Width, uint32_t Height);

	/**
	 * Transcode the payload into Dest, which must hold TranscodedSize() bytes and be readable (the
	 * library reads back what it wrote for some targets). Thread-safe: no state is shared between
	 * calls. bHighQuality selects the slower, better ASTC-family to BC7 path.
	 */
	bool Transcode(const void* Data, uint64_t Size, int32_t TranscoderTarget, void* Dest, uint64_t DestSize, bool bHighQuality);
}
