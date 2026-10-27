// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#include "BasisTranscoder.h"

#include "BasisTranscodeCore.h"
#include "Modules/ModuleManager.h"

#include "BasisUniversalBegin.h"
#include "transcoder/basisu_transcoder.h"
#include "BasisUniversalEnd.h"

namespace
{
	using basist::transcoder_texture_format;
	using UE::BasisTranscoder::GetTranscoderTarget;

	// The public mapping is written as plain numbers so that its header needs no library headers.
	static_assert(GetTranscoderTarget(PF_ETC2_RGB) == (int32)transcoder_texture_format::cTFETC1_RGB);
	static_assert(GetTranscoderTarget(PF_ETC2_RGBA) == (int32)transcoder_texture_format::cTFETC2_RGBA);
	static_assert(GetTranscoderTarget(PF_DXT1) == (int32)transcoder_texture_format::cTFBC1_RGB);
	static_assert(GetTranscoderTarget(PF_DXT5) == (int32)transcoder_texture_format::cTFBC3_RGBA);
	static_assert(GetTranscoderTarget(PF_BC4) == (int32)transcoder_texture_format::cTFBC4_R);
	static_assert(GetTranscoderTarget(PF_BC5) == (int32)transcoder_texture_format::cTFBC5_RG);
	static_assert(GetTranscoderTarget(PF_BC7) == (int32)transcoder_texture_format::cTFBC7_RGBA);
	static_assert(GetTranscoderTarget(PF_ETC2_R11_EAC) == (int32)transcoder_texture_format::cTFETC2_EAC_R11);
	static_assert(GetTranscoderTarget(PF_ETC2_RG11_EAC) == (int32)transcoder_texture_format::cTFETC2_EAC_RG11);
	static_assert(GetTranscoderTarget(PF_ASTC_4x4) == (int32)transcoder_texture_format::cTFASTC_LDR_4x4_RGBA);
	static_assert(GetTranscoderTarget(PF_ASTC_6x6) == (int32)transcoder_texture_format::cTFASTC_LDR_6x6_RGBA);
	static_assert(GetTranscoderTarget(PF_ASTC_8x8) == (int32)transcoder_texture_format::cTFASTC_LDR_8x8_RGBA);
	static_assert(GetTranscoderTarget(PF_ASTC_10x10) == (int32)transcoder_texture_format::cTFASTC_LDR_10x10_RGBA);
	static_assert(GetTranscoderTarget(PF_ASTC_12x12) == (int32)transcoder_texture_format::cTFASTC_LDR_12x12_RGBA);
	static_assert(GetTranscoderTarget(PF_BC6H) == (int32)transcoder_texture_format::cTFBC6H);
	static_assert(GetTranscoderTarget(PF_ASTC_4x4_HDR) == (int32)transcoder_texture_format::cTFASTC_HDR_4x4_RGBA);
	static_assert(GetTranscoderTarget(PF_ASTC_6x6_HDR) == (int32)transcoder_texture_format::cTFASTC_HDR_6x6_RGBA);

	class FBasisTranscoder final : public IBasisTranscoder
	{
	public:
		virtual bool GetPayloadInfo(FMemoryView Mip, FPayloadInfo& OutInfo) const override
		{
			BasisTranscodeCore::FPayloadInfo Info;
			if (!BasisTranscodeCore::GetPayloadInfo(Mip.GetData(), Mip.GetSize(), Info))
			{
				return false;
			}
			OutInfo.Width = Info.Width;
			OutInfo.Height = Info.Height;
			OutInfo.PayloadSize = Info.PayloadSize;
			OutInfo.bHasAlpha = Info.bHasAlpha;
			return true;
		}

		virtual bool CanTranscode(FMemoryView Mip, EPixelFormat Format) const override
		{
			return BasisTranscodeCore::CanTranscode(Mip.GetData(), Mip.GetSize(), GetTranscoderTarget(Format));
		}

		virtual bool Transcode(FMemoryView Mip, EPixelFormat Format, FMutableMemoryView Dest, bool bHighQuality) const override
		{
			return BasisTranscodeCore::Transcode(Mip.GetData(), Mip.GetSize(), GetTranscoderTarget(Format),
				Dest.GetData(), Dest.GetSize(), bHighQuality);
		}
	};
}

class FBasisTranscoderModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		BasisTranscodeCore::Init();
		IModularFeatures::Get().RegisterModularFeature(IBasisTranscoder::GetModularFeatureName(), &Transcoder);
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures::Get().UnregisterModularFeature(IBasisTranscoder::GetModularFeatureName(), &Transcoder);
	}

private:
	FBasisTranscoder Transcoder;
};

IMPLEMENT_MODULE(FBasisTranscoderModule, BasisTranscoder)
