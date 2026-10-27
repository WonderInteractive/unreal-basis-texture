// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#include "BasisEncoder.h"

#include "BasisUniversalBegin.h"
#include "encoder/basisu_bc15_spmd.h"
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_etc.h"
#include "transcoder/basisu_transcoder.h"
#include "BasisUniversalEnd.h"

#include <mutex>

namespace BasisEncoder
{
	namespace
	{
		using basist::basis_tex_format;
		using basist::transcoder_texture_format;

		constexpr uint32_t NumAstcBlockSizes = 14;

		bool IsValidTarget(int32_t Target)
		{
			return Target >= 0 && Target < (int32_t)transcoder_texture_format::cTFTotalTextureFormats
				&& !basist::basis_transcoder_format_is_uncompressed((transcoder_texture_format)Target)
				&& !basist::basis_transcoder_format_is_hdr((transcoder_texture_format)Target);
		}

		bool IsAstcLdrTarget(transcoder_texture_format Target)
		{
			return Target == transcoder_texture_format::cTFASTC_LDR_4x4_RGBA
				|| (Target >= transcoder_texture_format::cTFASTC_LDR_5x4_RGBA && Target <= transcoder_texture_format::cTFASTC_LDR_12x12_RGBA);
		}

		/** The member of an ASTC-block-size family (starting at First) with the given block size, or -1. */
		int32_t FindBlockSizeVariant(basis_tex_format First, uint32_t BlockWidth, uint32_t BlockHeight)
		{
			for (uint32_t Index = 0; Index < NumAstcBlockSizes; ++Index)
			{
				const basis_tex_format Candidate = (basis_tex_format)((uint32_t)First + Index);
				uint32_t W = 0, H = 0;
				basist::get_basis_tex_format_block_size(Candidate, W, H);
				if (W == BlockWidth && H == BlockHeight)
				{
					return (int32_t)Candidate;
				}
			}
			return -1;
		}

		int32_t PayloadFormat(const FSettings& Settings)
		{
			const transcoder_texture_format Target = (transcoder_texture_format)Settings.TranscoderTarget;
			uint32_t BlockWidth = Settings.PayloadBlockWidth;
			uint32_t BlockHeight = Settings.PayloadBlockHeight;
			if (IsAstcLdrTarget(Target))
			{
				// ASTC can only be reached from its own block size.
				BlockWidth = basist::basis_get_block_width(Target);
				BlockHeight = basist::basis_get_block_height(Target);
			}
			if (Target == transcoder_texture_format::cTFBC7_RGBA && Settings.bBC7PayloadAsXUBC7)
			{
				// Supercompressed BC7: the device copies blocks out rather than encoding them.
				return (int32_t)basis_tex_format::cXUBC7;
			}
			const int32_t Format = FindBlockSizeVariant(basis_tex_format::cXUASTC_LDR_4x4, BlockWidth, BlockHeight);
			if (Format < 0 || !basist::basis_is_format_supported(Target, (basis_tex_format)Format))
			{
				return -1;
			}
			return Format;
		}

		/**
		 * Native targets encoded straight from the source rather than transcoded from an
		 * intermediate. On photographs bc_spmd's BC1 and BC3 beat the best intermediate's transcode
		 * by 0.15-0.6 dB and its BC4 and BC5 by 0.85-2.2 dB (UASTC's by 1-3 dB), and a direct EAC
		 * encode beats UASTC's R11 and RG11 by 1.3-3.8 dB, each in a fraction of the time.
		 */
		bool IsDirectTarget(transcoder_texture_format Target)
		{
			switch (Target)
			{
			case transcoder_texture_format::cTFBC1_RGB:
			case transcoder_texture_format::cTFBC3_RGBA:
			case transcoder_texture_format::cTFBC4_R:
			case transcoder_texture_format::cTFBC5_RG:
			case transcoder_texture_format::cTFETC2_EAC_R11:
			case transcoder_texture_format::cTFETC2_EAC_RG11:
				return true;
			default:
				return false;
			}
		}

		int32_t NativeFormat(const FSettings& Settings)
		{
			const transcoder_texture_format Target = (transcoder_texture_format)Settings.TranscoderTarget;
			if (IsDirectTarget(Target))
			{
				return -1;
			}
			if (IsAstcLdrTarget(Target))
			{
				// Encode ASTC directly at the destination's block size: the transcode is then a copy,
				// and the encoder can use every ASTC feature rather than UASTC's subset.
				return FindBlockSizeVariant(basis_tex_format::cASTC_LDR_4x4,
					basist::basis_get_block_width(Target), basist::basis_get_block_height(Target));
			}
			if (Target == transcoder_texture_format::cTFBC7_RGBA)
			{
				// XUBC7 at quality 100 is lossless BC7, so this is a full BC7 encode: better than BC7
				// transcoded from UASTC on photographic content, and several times faster.
				return (int32_t)basis_tex_format::cXUBC7;
			}
			// UASTC LDR 4x4 reaches every other LDR block format at high quality. For ETC1 colour it
			// also carries per-block ETC1 hints, which beat a direct ETC1 encode with basisu's optimizer.
			return (int32_t)basis_tex_format::cUASTC_LDR_4x4;
		}

		/** EAC search radius around the initial base and multiplier, from the effort: 1 takes about
		 *  as long as the UASTC route, and 3 adds 0.3-0.4 dB at three times the time. */
		uint32_t EacSearchRadius(const FSettings& Settings)
		{
			const int32_t Effort = Settings.Effort < 0 ? 3 : Settings.Effort;
			return Effort <= 1 ? 1u : (Effort == 2 ? 2u : 3u);
		}

		bool LayoutUsesAlpha(const FSettings& Settings)
		{
			switch (Settings.Layout)
			{
			case EChannelLayout::RGBA: return Settings.bHasAlpha;
			case EChannelLayout::RRRG: return true;
			case EChannelLayout::AGR:  return true;
			default:                   return false;
			}
		}

		void FillSlice(const uint8_t* BGRA, uint32_t Width, uint32_t Height, const FSettings& Settings, basisu::image& Out)
		{
			Out.resize(Width, Height);
			basisu::color_rgba* Dest = Out.get_ptr();
			const uint64_t NumPixels = (uint64_t)Width * Height;
			const bool bOpaque = !Settings.bHasAlpha;
			for (uint64_t Index = 0; Index < NumPixels; ++Index)
			{
				const uint8_t B = BGRA[Index * 4 + 0];
				const uint8_t G = BGRA[Index * 4 + 1];
				const uint8_t R = BGRA[Index * 4 + 2];
				const uint8_t A = BGRA[Index * 4 + 3];
				switch (Settings.Layout)
				{
				case EChannelLayout::RGBA: Dest[Index].set(R, G, B, bOpaque ? 255 : A); break;
				case EChannelLayout::RRRG: Dest[Index].set(R, R, R, G); break;
				case EChannelLayout::RRR1: Dest[Index].set(R, R, R, 255); break;
				case EChannelLayout::RG01: Dest[Index].set(R, G, 0, 255); break;
				case EChannelLayout::AGR:  Dest[Index].set(255, G, 0, R); break;
				}
			}
		}

		const char* ErrorName(basisu::basis_compressor::error_code Code)
		{
			using EC = basisu::basis_compressor::error_code;
			switch (Code)
			{
			case EC::cECSuccess:                    return "success";
			case EC::cECFailedInitializing:         return "failed initializing";
			case EC::cECFailedReadingSourceImages:  return "failed reading source images";
			case EC::cECFailedValidating:           return "failed validating";
			case EC::cECFailedEncodeUASTC:          return "failed encoding";
			case EC::cECFailedFrontEnd:             return "failed in front end";
			case EC::cECFailedFrontendExtract:      return "failed extracting front end data";
			case EC::cECFailedBackend:              return "failed in back end";
			case EC::cECFailedCreateBasisFile:      return "failed creating .basis file";
			case EC::cECFailedWritingOutput:        return "failed writing output";
			case EC::cECFailedUASTCRDOPostProcess:  return "failed in RDO post-process";
			case EC::cECFailedCreateKTX2File:       return "failed creating KTX2 file";
			case EC::cECFailedInvalidParameters:    return "invalid parameters";
			}
			return "unknown error";
		}

		bool TranscodeNative(const basisu::uint8_vec& File, uint32_t Width, uint32_t Height, uint32_t NumSlices, transcoder_texture_format Target, FResult& Out)
		{
			const uint32_t BlockWidth = basist::basis_get_block_width(Target);
			const uint32_t BlockHeight = basist::basis_get_block_height(Target);
			const uint32_t NumBlocks = ((Width + BlockWidth - 1) / BlockWidth) * ((Height + BlockHeight - 1) / BlockHeight);
			const uint64_t SliceSize = NativeSliceSize((int32_t)Target, Width, Height);

			basist::basisu_transcoder Transcoder;
			if (!Transcoder.start_transcoding(File.data(), (uint32_t)File.size()))
			{
				Out.Error = "start_transcoding failed";
				return false;
			}

			Out.Data.resize((size_t)(SliceSize * NumSlices));
			for (uint32_t Slice = 0; Slice < NumSlices; ++Slice)
			{
				// HighQuality only changes the ASTC-family to BC7 path; cook time can afford it.
				if (!Transcoder.transcode_image_level(File.data(), (uint32_t)File.size(), Slice, 0,
					Out.Data.data() + SliceSize * Slice, NumBlocks, Target, basist::cDecodeFlagsHighQuality))
				{
					Out.Error = "transcode to " + std::string(basist::basis_get_format_name(Target)) + " failed for slice " + std::to_string(Slice);
					return false;
				}
			}
			return true;
		}

		/** EAC-encodes one channel of Image into the 8-byte blocks at Dest, Stride bytes apart. */
		void EncodeEacChannel(const basisu::image& Image, uint32_t Channel, uint32_t Radius, uint8_t* Dest, uint32_t Stride)
		{
			const uint32_t BlocksX = (Image.get_width() + 3) / 4;
			const uint32_t BlocksY = (Image.get_height() + 3) / 4;
			for (uint32_t BlockY = 0; BlockY < BlocksY; ++BlockY)
			{
				for (uint32_t BlockX = 0; BlockX < BlocksX; ++BlockX)
				{
					uint8_t Values[16];
					for (uint32_t Y = 0; Y < 4; ++Y)
					{
						for (uint32_t X = 0; X < 4; ++X)
						{
							Values[Y * 4 + X] = Image.get_clamped((int)(BlockX * 4 + X), (int)(BlockY * 4 + Y))[Channel];
						}
					}
					// R11 shares A8's block layout, and pack_eac_a8 never picks multiplier 0 (the one
					// value the two decode differently), so the block reads as either.
					uint8_t* Block = Dest + ((uint64_t)BlockY * BlocksX + BlockX) * Stride;
					basisu::pack_eac_a8(reinterpret_cast<basisu::eac_a8_block*>(Block), Values, Radius, Radius);
				}
			}
		}

		/** BC1-5 with bc_spmd, which takes each block's 16 pixels contiguously. Its BC1 blocks are
		 *  always four-colour, so none decode to transparent black. */
		void EncodeBc(const basisu::image& Image, transcoder_texture_format Target, uint8_t* Dest)
		{
			const uint32_t BlocksX = (Image.get_width() + 3) / 4;
			const uint32_t BlocksY = (Image.get_height() + 3) / 4;
			const uint32_t NumBlocks = BlocksX * BlocksY;
			std::vector<basisu::color_rgba> Pixels((size_t)NumBlocks * 16);
			for (uint32_t BlockY = 0; BlockY < BlocksY; ++BlockY)
			{
				for (uint32_t BlockX = 0; BlockX < BlocksX; ++BlockX)
				{
					basisu::color_rgba* Block = Pixels.data() + ((size_t)BlockY * BlocksX + BlockX) * 16;
					for (uint32_t Y = 0; Y < 4; ++Y)
					{
						for (uint32_t X = 0; X < 4; ++X)
						{
							Block[Y * 4 + X] = Image.get_clamped((int)(BlockX * 4 + X), (int)(BlockY * 4 + Y));
						}
					}
				}
			}

			// high_quality adds a least-squares endpoint refit to the BC4 blocks (BC3 alpha, BC4,
			// BC5): about 0.7 dB for a fraction of a millisecond per thousand blocks.
			switch (Target)
			{
			case transcoder_texture_format::cTFBC1_RGB:
				basisu::bc_spmd::encode_bc1(Dest, Pixels.data(), NumBlocks);
				break;
			case transcoder_texture_format::cTFBC3_RGBA:
				basisu::bc_spmd::encode_bc3(Dest, Pixels.data(), NumBlocks, true, true);
				break;
			case transcoder_texture_format::cTFBC4_R:
				basisu::bc_spmd::encode_bc4(Dest, Pixels.data(), NumBlocks, true, true);
				break;
			default:
				// BC5 reads R and G; the RRRG layout carries the second channel in alpha.
				for (basisu::color_rgba& Pixel : Pixels)
				{
					Pixel.g = Pixel.a;
				}
				basisu::bc_spmd::encode_bc5(Dest, Pixels.data(), NumBlocks, true, true);
				break;
			}
		}

		bool EncodeDirect(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out)
		{
			const transcoder_texture_format Target = (transcoder_texture_format)Settings.TranscoderTarget;
			const uint64_t SliceSize = NativeSliceSize(Settings.TranscoderTarget, Width, Height);
			const uint64_t SliceBytes = (uint64_t)Width * Height * 4;
			const uint32_t Radius = EacSearchRadius(Settings);

			Out.Data.resize((size_t)(SliceSize * NumSlices));
			basisu::image Image;
			for (uint32_t Slice = 0; Slice < NumSlices; ++Slice)
			{
				FillSlice(BGRA + SliceBytes * Slice, Width, Height, Settings, Image);
				uint8_t* Dest = Out.Data.data() + SliceSize * Slice;
				switch (Target)
				{
				case transcoder_texture_format::cTFETC2_EAC_R11:
					EncodeEacChannel(Image, 0, Radius, Dest, 8);
					break;
				case transcoder_texture_format::cTFETC2_EAC_RG11:
					EncodeEacChannel(Image, 0, Radius, Dest, 16);
					EncodeEacChannel(Image, 3, Radius, Dest + 8, 16);
					break;
				default:
					EncodeBc(Image, Target, Dest);
					break;
				}
			}
			return true;
		}

		/** Encodes the slices to a .basis file in Format. */
		bool EncodeBasis(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings,
			basis_tex_format Format, int32_t Quality, basisu::uint8_vec& File, FResult& Out)
		{
			basisu::basis_compressor_params Params;
			if (!Params.set_format_mode_and_quality_effort(Format, Quality, Settings.Effort))
			{
				Out.Error = std::string("could not configure ") + basist::basis_get_tex_format_name(Format);
				return false;
			}
			Params.set_srgb_options(Settings.bSRGB);
			Params.m_check_for_alpha = LayoutUsesAlpha(Settings);
			Params.m_mip_gen = false; // Unreal provides each mip as its own image
			Params.m_tex_type = NumSlices > 1 ? basist::cBASISTexType2DArray : basist::cBASISTexType2D;
			Params.m_status_output = false;
			Params.m_compute_stats = false;
			Params.m_print_stats = false;
			// Unreal builds many textures in parallel, so each encode stays on its calling thread.
			Params.m_multithreading = false;
			basisu::job_pool JobPool(1);
			Params.m_pJob_pool = &JobPool;

			Params.m_source_images.resize(NumSlices);
			const uint64_t SliceBytes = (uint64_t)Width * Height * 4;
			for (uint32_t Slice = 0; Slice < NumSlices; ++Slice)
			{
				FillSlice(BGRA + SliceBytes * Slice, Width, Height, Settings, Params.m_source_images[Slice]);
			}

			basisu::basis_compressor Compressor;
			if (!Compressor.init(Params))
			{
				Out.Error = "basis_compressor::init failed";
				return false;
			}
			const basisu::basis_compressor::error_code Result = Compressor.process();
			if (Result != basisu::basis_compressor::cECSuccess)
			{
				Out.Error = std::string(basist::basis_get_tex_format_name(Format)) + " encode " + ErrorName(Result);
				return false;
			}
			File = Compressor.get_output_basis_file();
			return true;
		}

		bool EncodeNative(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out)
		{
			const transcoder_texture_format Target = (transcoder_texture_format)Settings.TranscoderTarget;
			if (IsDirectTarget(Target))
			{
				return EncodeDirect(BGRA, Width, Height, NumSlices, Settings, Out);
			}

			const int32_t Format = NativeFormat(Settings);
			if (Format < 0 || !basist::basis_is_format_supported(Target, (basis_tex_format)Format))
			{
				Out.Error = std::string("no Basis intermediate reaches ") + basist::basis_get_format_name(Target);
				return false;
			}
			basisu::uint8_vec File;
			if (!EncodeBasis(BGRA, Width, Height, NumSlices, Settings, (basis_tex_format)Format, 100, File, Out)
				|| !TranscodeNative(File, Width, Height, NumSlices, Target, Out))
			{
				return false;
			}

			if (Target == transcoder_texture_format::cTFETC2_RGBA && LayoutUsesAlpha(Settings))
			{
				// The transcoder re-encodes alpha from UASTC's decoded pixels; encoding it from the
				// source instead skips that generation loss.
				const uint64_t SliceSize = NativeSliceSize(Settings.TranscoderTarget, Width, Height);
				const uint64_t SliceBytes = (uint64_t)Width * Height * 4;
				basisu::image Image;
				for (uint32_t Slice = 0; Slice < NumSlices; ++Slice)
				{
					FillSlice(BGRA + SliceBytes * Slice, Width, Height, Settings, Image);
					EncodeEacChannel(Image, 3, EacSearchRadius(Settings), Out.Data.data() + SliceSize * Slice, 16);
				}
			}
			return true;
		}
	}

	void Init()
	{
		static std::once_flag Once;
		std::call_once(Once, []()
		{
			basisu::basisu_encoder_init(false, false);
			basisu::bc_spmd::init();
		});
	}

	uint64_t NativeSliceSize(int32_t TranscoderTarget, uint32_t Width, uint32_t Height)
	{
		if (!IsValidTarget(TranscoderTarget))
		{
			return 0;
		}
		return basist::basis_compute_transcoded_image_size_in_bytes((transcoder_texture_format)TranscoderTarget, Width, Height);
	}

	bool WantsPayload(const FSettings& Settings)
	{
		return Settings.Output == EOutput::Payload && IsValidTarget(Settings.TranscoderTarget) && PayloadFormat(Settings) >= 0;
	}

	int32_t IntermediateFormat(const FSettings& Settings)
	{
		if (!IsValidTarget(Settings.TranscoderTarget))
		{
			return -1;
		}
		return WantsPayload(Settings) ? PayloadFormat(Settings) : NativeFormat(Settings);
	}

	const char* IntermediateFormatName(int32_t BasisTexFormat)
	{
		return BasisTexFormat < 0 ? "none" : basist::basis_get_tex_format_name((basis_tex_format)BasisTexFormat);
	}

	const char* TargetFormatName(int32_t TranscoderTarget)
	{
		return IsValidTarget(TranscoderTarget) ? basist::basis_get_format_name((transcoder_texture_format)TranscoderTarget) : "invalid";
	}

	const char* LibraryVersion()
	{
		return BASISU_LIB_VERSION_STRING;
	}

	bool Encode(const uint8_t* BGRA, uint32_t Width, uint32_t Height, uint32_t NumSlices, const FSettings& Settings, FResult& Out)
	{
		Out = FResult();

		if (BGRA == nullptr || Width == 0 || Height == 0 || NumSlices == 0)
		{
			Out.Error = "empty image";
			return false;
		}
		if (!IsValidTarget(Settings.TranscoderTarget))
		{
			Out.Error = "unsupported destination format " + std::to_string(Settings.TranscoderTarget);
			return false;
		}

		Init();

		const transcoder_texture_format Target = (transcoder_texture_format)Settings.TranscoderTarget;
		bool bPayload = WantsPayload(Settings);
		if (bPayload && NumSlices != 1)
		{
			// One payload per slice would need a per-slice container the runtime does not define.
			bPayload = false;
			Out.Note = "payload output is single-slice only; encoded native blocks";
		}

		if (bPayload)
		{
			const int32_t Format = PayloadFormat(Settings);
			if (!basist::basis_is_format_supported(Target, (basis_tex_format)Format))
			{
				Out.Error = std::string(basist::basis_get_tex_format_name((basis_tex_format)Format)) + " does not reach " + basist::basis_get_format_name(Target);
				return false;
			}
			const int32_t Quality = Settings.Quality < 1 ? 1 : (Settings.Quality > 100 ? 100 : Settings.Quality);
			basisu::uint8_vec File;
			if (!EncodeBasis(BGRA, Width, Height, 1, Settings, (basis_tex_format)Format, Quality, File, Out))
			{
				return false;
			}

			// The device receives a buffer sized for the native mip; a payload that does not fit
			// cannot be delivered intact, so it falls back to native blocks, encoded afresh rather
			// than transcoded from the (possibly lossy) payload.
			const uint64_t NativeSize = NativeSliceSize(Settings.TranscoderTarget, Width, Height);
			if (File.size() <= NativeSize)
			{
				Out.Data.assign(File.data(), File.data() + File.size());
				Out.bPayload = true;
				return true;
			}
			Out.Note = "payload of " + std::to_string(File.size()) + " bytes exceeds the " + std::to_string(NativeSize)
				+ " native bytes; encoded native blocks";
		}

		return EncodeNative(BGRA, Width, Height, NumSlices, Settings, Out);
	}
}
