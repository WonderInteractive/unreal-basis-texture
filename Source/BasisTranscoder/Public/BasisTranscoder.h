// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreTypes.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"
#include "Memory/MemoryView.h"
#include "PixelFormat.h"
#include "UObject/NameTypes.h"

/**
 * Turns the payloads the TextureFormatBasisPayload encoder cooks into native blocks on the device,
 * when the mip is uploaded.
 *
 * A payload mip is exactly the size of the native mip it stands for: a .basis file at its start,
 * zeros after it (which pak compression removes). Upload code asks GetPayloadInfo of each mip of a
 * Basis-cooked texture and, for a payload, transcodes it into a native-sized buffer and uploads
 * that instead. Mips the encoder left native (too small to gain, or not a plain 2D texture) fail
 * GetPayloadInfo and upload as they are.
 *
 * Header-only on purpose. An engine module such as an RHI cannot depend on a plugin module, but it
 * can include this header (add the plugin's Source/BasisTranscoder/Public to its include paths) and
 * find the implementation at runtime as a modular feature. Plugin and game modules can depend on
 * BasisTranscoder and use it the same way.
 */
class IBasisTranscoder : public IModularFeature
{
public:
	static FName GetModularFeatureName()
	{
		static const FName Name(TEXT("BasisTranscoder"));
		return Name;
	}

	/**
	 * The transcoder, or null when the BasisTranscoder module is not loaded. Call it on the game
	 * thread (during RHI initialisation, say) and keep the pointer, which stays valid until the
	 * module shuts down. The methods themselves are thread-safe.
	 */
	static IBasisTranscoder* Get()
	{
		IModularFeatures& Features = IModularFeatures::Get();
		if (!Features.IsModularFeatureAvailable(GetModularFeatureName()))
		{
			return nullptr;
		}
		return &Features.GetModularFeature<IBasisTranscoder>(GetModularFeatureName());
	}

	struct FPayloadInfo
	{
		uint32 Width = 0;
		uint32 Height = 0;
		/** Bytes the payload occupies at the start of the mip. */
		uint32 PayloadSize = 0;
		bool bHasAlpha = false;
	};

	/**
	 * Whether Mip starts with a payload, and if so its description. Checks the header and its
	 * checksum only: cheap, and safe on any bytes, native blocks included.
	 */
	virtual bool GetPayloadInfo(FMemoryView Mip, FPayloadInfo& OutInfo) const = 0;

	/** Whether the payload at the start of Mip can become Format's blocks. */
	virtual bool CanTranscode(FMemoryView Mip, EPixelFormat Format) const = 0;

	/**
	 * Transcode the payload at the start of Mip into Format's native blocks in Dest, which must
	 * hold the whole native mip and be readable. Returns false if Mip holds no payload, the payload
	 * cannot reach Format, or Dest is too small. bHighQuality takes the slower, better path from an
	 * XUASTC payload to BC7, and changes nothing else.
	 */
	virtual bool Transcode(FMemoryView Mip, EPixelFormat Format, FMutableMemoryView Dest, bool bHighQuality = false) const = 0;

	virtual ~IBasisTranscoder() = default;
};

namespace UE::BasisTranscoder
{
	/**
	 * The basis_universal transcoder target (a basist::transcoder_texture_format value) for an
	 * Unreal pixel format, or -1 for a format the plugin does not produce. The encoder and the
	 * runtime both use it, so they always agree; BasisTranscoder.cpp holds the numbers to the
	 * library's.
	 */
	constexpr int32 GetTranscoderTarget(EPixelFormat Format)
	{
		switch (Format)
		{
		case PF_ETC2_RGB:           return 0;  // cTFETC1_RGB: ETC1 blocks are valid ETC2 RGB blocks
		case PF_ETC2_RGBA:          return 1;  // cTFETC2_RGBA
		case PF_DXT1:               return 2;  // cTFBC1_RGB
		case PF_DXT5:               return 3;  // cTFBC3_RGBA
		case PF_BC4:                return 4;  // cTFBC4_R
		case PF_BC5:                return 5;  // cTFBC5_RG
		case PF_BC7:                return 6;  // cTFBC7_RGBA
		case PF_ETC2_R11_EAC:       return 20; // cTFETC2_EAC_R11
		case PF_ETC2_RG11_EAC:      return 21; // cTFETC2_EAC_RG11
		// The NORM_RG variants hold the same blocks, with X and Y in luminance and alpha.
		case PF_ASTC_4x4:
		case PF_ASTC_4x4_NORM_RG:   return 10; // cTFASTC_LDR_4x4_RGBA
		case PF_ASTC_6x6:
		case PF_ASTC_6x6_NORM_RG:   return 31; // cTFASTC_LDR_6x6_RGBA
		case PF_ASTC_8x8:
		case PF_ASTC_8x8_NORM_RG:   return 36; // cTFASTC_LDR_8x8_RGBA
		case PF_ASTC_10x10:
		case PF_ASTC_10x10_NORM_RG: return 38; // cTFASTC_LDR_10x10_RGBA
		case PF_ASTC_12x12:
		case PF_ASTC_12x12_NORM_RG: return 40; // cTFASTC_LDR_12x12_RGBA
		default:                    return -1;
		}
	}
}
