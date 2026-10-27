// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#include "BasisTranscodeCore.h"

#include "BasisUniversalBegin.h"
#include "transcoder/basisu_transcoder.h"
#include "BasisUniversalEnd.h"

namespace BasisTranscodeCore
{
	namespace
	{
		using basist::transcoder_texture_format;

		uint32_t ClampSize(uint64_t Size)
		{
			return Size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)Size;
		}

		bool IsBlockTarget(int32_t Target)
		{
			return Target >= 0 && Target < (int32_t)transcoder_texture_format::cTFTotalTextureFormats
				&& !basist::basis_transcoder_format_is_uncompressed((transcoder_texture_format)Target);
		}
	}

	void Init()
	{
		// A function-local static is initialised exactly once, even under concurrent first calls;
		// basisu_transcoder_init() itself is not safe to race.
		static const bool bInitialized = []()
		{
			basist::basisu_transcoder_init();
			return true;
		}();
		(void)bInitialized;
	}

	bool GetPayloadInfo(const void* Data, uint64_t Size, FPayloadInfo& Out)
	{
		if (Data == nullptr)
		{
			return false;
		}
		Init();

		const uint32_t DataSize = ClampSize(Size);
		const basist::basisu_transcoder Transcoder;
		if (!Transcoder.validate_file_checksums(Data, DataSize, false)
			|| Transcoder.get_total_images(Data, DataSize) != 1
			|| Transcoder.get_total_image_levels(Data, DataSize, 0) != 1)
		{
			return false;
		}

		basist::basisu_image_info Info;
		if (!Transcoder.get_image_info(Data, DataSize, Info, 0))
		{
			return false;
		}

		Out.Width = Info.m_orig_width;
		Out.Height = Info.m_orig_height;
		Out.bHasAlpha = Info.m_alpha_flag;
		Out.SourceFormat = (int32_t)Transcoder.get_basis_tex_format(Data, DataSize);
		Out.PayloadSize = (uint32_t)sizeof(basist::basis_file_header)
			+ reinterpret_cast<const basist::basis_file_header*>(Data)->m_data_size;
		return true;
	}

	bool CanTranscode(const void* Data, uint64_t Size, int32_t TranscoderTarget)
	{
		FPayloadInfo Info;
		return IsBlockTarget(TranscoderTarget) && GetPayloadInfo(Data, Size, Info)
			&& basist::basis_is_format_supported((transcoder_texture_format)TranscoderTarget, (basist::basis_tex_format)Info.SourceFormat);
	}

	uint64_t TranscodedSize(int32_t TranscoderTarget, uint32_t Width, uint32_t Height)
	{
		if (!IsBlockTarget(TranscoderTarget))
		{
			return 0;
		}
		return basist::basis_compute_transcoded_image_size_in_bytes((transcoder_texture_format)TranscoderTarget, Width, Height);
	}

	bool Transcode(const void* Data, uint64_t Size, int32_t TranscoderTarget, void* Dest, uint64_t DestSize, bool bHighQuality)
	{
		FPayloadInfo Info;
		if (Dest == nullptr || !IsBlockTarget(TranscoderTarget) || !GetPayloadInfo(Data, Size, Info))
		{
			return false;
		}

		const transcoder_texture_format Target = (transcoder_texture_format)TranscoderTarget;
		if (!basist::basis_is_format_supported(Target, (basist::basis_tex_format)Info.SourceFormat)
			|| DestSize < TranscodedSize(TranscoderTarget, Info.Width, Info.Height))
		{
			return false;
		}

		// The output is counted in DESTINATION blocks. The level info's block count describes the
		// source's grid, which differs whenever the intermediate's block size does (a 6x6 payload
		// to a 4x4 format), and the library rejects a buffer counted that way.
		const uint32_t BlockWidth = basist::basis_get_block_width(Target);
		const uint32_t BlockHeight = basist::basis_get_block_height(Target);
		const uint32_t NumBlocks = ((Info.Width + BlockWidth - 1) / BlockWidth) * ((Info.Height + BlockHeight - 1) / BlockHeight);

		// start_transcoding() holds per-file state, so each call gets its own transcoder.
		basist::basisu_transcoder Transcoder;
		if (!Transcoder.start_transcoding(Data, Info.PayloadSize))
		{
			return false;
		}
		// From XUASTC to BC7, the high-quality flag's gain (about 0.6 dB) comes from skipping the
		// fast path, which alone costs 1.4x the transcode time instead of 1.7x.
		uint32_t Flags = 0;
		if (bHighQuality)
		{
			Flags = Target == transcoder_texture_format::cTFBC7_RGBA && basist::basis_tex_format_is_xuastc_ldr((basist::basis_tex_format)Info.SourceFormat)
				? basist::cDecodeFlagXUASTCLDRDisableFastBC7Transcoding
				: basist::cDecodeFlagsHighQuality;
		}
		return Transcoder.transcode_image_level(Data, Info.PayloadSize, 0, 0, Dest, NumBlocks, Target, Flags);
	}
}
