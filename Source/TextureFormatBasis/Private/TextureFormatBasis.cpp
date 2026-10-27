// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#include "TextureFormatBasis.h"

#include "BasisEncoder.h"
#include "BasisTranscoder.h"
#include "DerivedDataBuildFunctionFactory.h"
#include "DerivedDataBuildVersion.h"
#include "ImageCore.h"
#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatModule.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"
#include "TextureBuildFunction.h"
#include "TextureCompressorModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogTextureFormatBasis, Log, All);

namespace UE::TextureFormatBasis::Private
{
	using BasisEncoder::EChannelLayout;

	// Bump when the encoded output changes for the same input and settings. The vendored library's
	// version is in both cache keys already (GetDerivedDataKeyString, AddBuildVersion).
	constexpr uint16 FormatVersion = 3; // 2: BC1-5 and EAC encoded directly; 3: linear channel weights, payload coding

	enum class EBlockSize : uint8
	{
		Fixed,       // the format's own pixel format
		ASTC,        // ASTC block size by quality, as the engine's ASTC encoders choose it
		ASTCHQ,      // the same, from the HQ default
		ASTCNormal,  // ASTC 6x6, as the engine's ASTC encoders use for every normal map
		ASTCHDR,     // ASTC HDR 4x4 or 6x6 by quality: Basis has no larger HDR blocks
	};

	struct FFormat
	{
		/** The engine's format name, without the plugin's prefix. */
		const TCHAR* Name;
		EPixelFormat Opaque;
		/** For an image with alpha; the same as Opaque unless the format is an "Auto" one. */
		EPixelFormat WithAlpha;
		EChannelLayout Layout;
		/** Whether an RGBA-layout format keeps the image's alpha; false forces it opaque. */
		bool bKeepsAlpha;
		EBlockSize BlockSize;
	};

	// The engine format names this plugin encodes, and how. A platform's format names reach the
	// plugin only if they are listed here (the engine applies an alternate format's prefix only to
	// names it supports). Everything else stays with the engine's encoders: DXT3 (no BC2 in Basis)
	// and the uncompressed formats.
	const FFormat GFormats[] =
	{
		// The engine's TextureFormatDXT names, and BC7.
		{ TEXT("DXT1"),                  PF_DXT1,           PF_DXT1,           EChannelLayout::RGBA, false, EBlockSize::Fixed },
		{ TEXT("DXT5"),                  PF_DXT5,           PF_DXT5,           EChannelLayout::RGBA, true,  EBlockSize::Fixed },
		{ TEXT("AutoDXT"),               PF_DXT1,           PF_DXT5,           EChannelLayout::RGBA, true,  EBlockSize::Fixed },
		{ TEXT("DXT5n"),                 PF_DXT5,           PF_DXT5,           EChannelLayout::AGR,  false, EBlockSize::Fixed },
		{ TEXT("BC4"),                   PF_BC4,            PF_BC4,            EChannelLayout::RRR1, false, EBlockSize::Fixed },
		{ TEXT("BC5"),                   PF_BC5,            PF_BC5,            EChannelLayout::RRRG, false, EBlockSize::Fixed },
		{ TEXT("BC7"),                   PF_BC7,            PF_BC7,            EChannelLayout::RGBA, true,  EBlockSize::Fixed },
		// TextureFormatETC2's.
		{ TEXT("ETC2_RGB"),              PF_ETC2_RGB,       PF_ETC2_RGB,       EChannelLayout::RGBA, false, EBlockSize::Fixed },
		{ TEXT("ETC2_RGBA"),             PF_ETC2_RGBA,      PF_ETC2_RGBA,      EChannelLayout::RGBA, true,  EBlockSize::Fixed },
		{ TEXT("AutoETC2"),              PF_ETC2_RGB,       PF_ETC2_RGBA,      EChannelLayout::RGBA, true,  EBlockSize::Fixed },
		{ TEXT("ETC2_R11"),              PF_ETC2_R11_EAC,   PF_ETC2_R11_EAC,   EChannelLayout::RRR1, false, EBlockSize::Fixed },
		{ TEXT("ETC2_RG11"),             PF_ETC2_RG11_EAC,  PF_ETC2_RG11_EAC,  EChannelLayout::RRRG, false, EBlockSize::Fixed },
		// TextureFormatASTC's LDR names.
		{ TEXT("ASTC_RGB"),              PF_Unknown,        PF_Unknown,        EChannelLayout::RGBA, false, EBlockSize::ASTC },
		{ TEXT("ASTC_RGBA"),             PF_Unknown,        PF_Unknown,        EChannelLayout::RGBA, true,  EBlockSize::ASTC },
		{ TEXT("ASTC_RGBAuto"),          PF_Unknown,        PF_Unknown,        EChannelLayout::RGBA, true,  EBlockSize::ASTC },
		{ TEXT("ASTC_RGBA_HQ"),          PF_Unknown,        PF_Unknown,        EChannelLayout::RGBA, true,  EBlockSize::ASTCHQ },
		{ TEXT("ASTC_NormalAG"),         PF_ASTC_6x6,       PF_ASTC_6x6,       EChannelLayout::AGR,  false, EBlockSize::ASTCNormal },
		{ TEXT("ASTC_NormalRG"),         PF_ASTC_6x6,       PF_ASTC_6x6,       EChannelLayout::RG01, false, EBlockSize::ASTCNormal },
		{ TEXT("ASTC_NormalLA"),         PF_ASTC_6x6,       PF_ASTC_6x6,       EChannelLayout::RRRG, false, EBlockSize::ASTCNormal },
		// Luminance-alpha blocks the RHI reads as RG (it must support PF_ASTC_*_NORM_RG).
		{ TEXT("ASTC_NormalRG_Precise"), PF_ASTC_6x6_NORM_RG, PF_ASTC_6x6_NORM_RG, EChannelLayout::RRRG, false, EBlockSize::ASTCNormal },
		// The HDR formats (unless bEncodeHDR is off): linear RGB, alpha opaque.
		{ TEXT("BC6H"),                  PF_BC6H,           PF_BC6H,           EChannelLayout::RGBA, false, EBlockSize::Fixed },
		{ TEXT("ASTC_RGB_HDR"),          PF_Unknown,        PF_Unknown,        EChannelLayout::RGBA, false, EBlockSize::ASTCHDR },
	};

	bool IsHDR(const FFormat& Format)
	{
		return Format.Opaque == PF_BC6H || Format.BlockSize == EBlockSize::ASTCHDR;
	}

	/** Whether the format's block size follows the engine's ASTC quality defaults. */
	bool UsesASTCQualityDefaults(const FFormat& Format)
	{
		return Format.BlockSize == EBlockSize::ASTC || Format.BlockSize == EBlockSize::ASTCHQ || Format.BlockSize == EBlockSize::ASTCHDR;
	}

	const FFormat* FindFormat(FName PrefixedName, const FString& Prefix)
	{
		FString Name = PrefixedName.ToString();
		if (!Name.RemoveFromStart(Prefix))
		{
			return nullptr;
		}
		for (const FFormat& Format : GFormats)
		{
			if (Name == Format.Name)
			{
				return &Format;
			}
		}
		return nullptr;
	}

	/**
	 * Everything that changes the output besides the build settings. Read from the editor's config
	 * once; a texture build worker has no config and receives it in
	 * FTextureBuildSettings::FormatConfigOverride instead (see ExportGlobalFormatConfig).
	 */
	struct FConfig
	{
		/** Encoder effort, 0-10, or -1 for each mode's library default (see BasisEncoder::FSettings). */
		int32 Effort = 1;
		/** Payload quality 1-100; 100 supercompresses losslessly. */
		int32 PayloadQuality = 100;
		/** The XUASTC block size of payloads for destinations other than ASTC. */
		int32 PayloadBlockWidth = 6;
		int32 PayloadBlockHeight = 6;
		/** BC7 payloads as XUBC7 rather than XUASTC. */
		bool bBC7PayloadAsXUBC7 = false;
		/** XUASTC payload entropy coding (BasisEncoder::EPayloadCoding). */
		int32 PayloadCoding = (int32)BasisEncoder::EPayloadCoding::Zstd;
		/** Whether the plugin takes the HDR formats; when false they stay with the engine's encoders. */
		bool bEncodeHDR = true;
		/** The engine's ASTC quality-by-size defaults, 0-4 (12x12 to 4x4). */
		int32 ASTCQualityBySize = 3;
		int32 ASTCQualityBySizeHQ = 4;

		static FConfig FromEngineConfig()
		{
			FConfig Config;
			const TCHAR* Section = TEXT("TextureFormatBasisSettings");
			GConfig->GetInt(Section, TEXT("Effort"), Config.Effort, GEngineIni);
			GConfig->GetInt(Section, TEXT("PayloadQuality"), Config.PayloadQuality, GEngineIni);
			FString BlockSize;
			if (GConfig->GetString(Section, TEXT("PayloadBlockSize"), BlockSize, GEngineIni))
			{
				FString Width, Height;
				if (!BlockSize.Split(TEXT("x"), &Width, &Height)
					|| !LexTryParseString(Config.PayloadBlockWidth, *Width)
					|| !LexTryParseString(Config.PayloadBlockHeight, *Height))
				{
					UE_LOG(LogTextureFormatBasis, Warning, TEXT("[%s] PayloadBlockSize=%s is not WxH; using 6x6"), Section, *BlockSize);
					Config.PayloadBlockWidth = Config.PayloadBlockHeight = 6;
				}
			}
			GConfig->GetBool(Section, TEXT("bBC7PayloadAsXUBC7"), Config.bBC7PayloadAsXUBC7, GEngineIni);
			FString Coding;
			if (GConfig->GetString(Section, TEXT("PayloadCoding"), Coding, GEngineIni))
			{
				if (Coding == TEXT("Zstd"))
				{
					Config.PayloadCoding = (int32)BasisEncoder::EPayloadCoding::Zstd;
				}
				else if (Coding == TEXT("Hybrid"))
				{
					Config.PayloadCoding = (int32)BasisEncoder::EPayloadCoding::Hybrid;
				}
				else if (Coding == TEXT("Arithmetic"))
				{
					Config.PayloadCoding = (int32)BasisEncoder::EPayloadCoding::Arithmetic;
				}
				else
				{
					UE_LOG(LogTextureFormatBasis, Warning, TEXT("[%s] PayloadCoding=%s is not Zstd, Hybrid or Arithmetic; using Zstd"), Section, *Coding);
				}
			}
			GConfig->GetBool(Section, TEXT("bEncodeHDR"), Config.bEncodeHDR, GEngineIni);

			// The settings and command-line switches the engine's ASTC encoders read, so the plugin
			// picks the block sizes they would.
			GConfig->GetInt(TEXT("/Script/UnrealEd.CookerSettings"), TEXT("DefaultASTCQualityBySize"), Config.ASTCQualityBySize, GEngineIni);
			GConfig->GetInt(TEXT("/Script/UnrealEd.CookerSettings"), TEXT("DefaultASTCQualityBySizeHQ"), Config.ASTCQualityBySizeHQ, GEngineIni);
			FParse::Value(FCommandLine::Get(), TEXT("-astcqualitybysize="), Config.ASTCQualityBySize);
			FParse::Value(FCommandLine::Get(), TEXT("-astcqualitybysizehq="), Config.ASTCQualityBySizeHQ);
			return Config.Sanitized();
		}

		static FConfig FromFormatConfig(FCbObjectView Object, const FConfig& Defaults)
		{
			FConfig Config;
			Config.Effort = Object.FindView("Effort").AsInt32(Defaults.Effort);
			Config.PayloadQuality = Object.FindView("PayloadQuality").AsInt32(Defaults.PayloadQuality);
			Config.PayloadBlockWidth = Object.FindView("PayloadBlockWidth").AsInt32(Defaults.PayloadBlockWidth);
			Config.PayloadBlockHeight = Object.FindView("PayloadBlockHeight").AsInt32(Defaults.PayloadBlockHeight);
			Config.bBC7PayloadAsXUBC7 = Object.FindView("bBC7PayloadAsXUBC7").AsBool(Defaults.bBC7PayloadAsXUBC7);
			Config.PayloadCoding = Object.FindView("PayloadCoding").AsInt32(Defaults.PayloadCoding);
			Config.ASTCQualityBySize = Object.FindView("DefaultASTCQualityBySize").AsInt32(Defaults.ASTCQualityBySize);
			Config.ASTCQualityBySizeHQ = Object.FindView("DefaultASTCQualityBySizeHQ").AsInt32(Defaults.ASTCQualityBySizeHQ);
			return Config.Sanitized();
		}

		FConfig Sanitized() const
		{
			FConfig Config = *this;
			Config.Effort = FMath::Clamp(Effort, -1, 10);
			Config.PayloadQuality = FMath::Clamp(PayloadQuality, 1, 100);
			Config.PayloadCoding = FMath::Clamp(PayloadCoding, 0, (int32)BasisEncoder::EPayloadCoding::Arithmetic);
			Config.ASTCQualityBySize = FMath::Clamp(ASTCQualityBySize, 0, 4);
			Config.ASTCQualityBySizeHQ = FMath::Clamp(ASTCQualityBySizeHQ, 0, 4);
			return Config;
		}
	};

	EPixelFormat ASTCForQuality(int32 Quality)
	{
		switch (Quality)
		{
		case 0:  return PF_ASTC_12x12;
		case 1:  return PF_ASTC_10x10;
		case 2:  return PF_ASTC_8x8;
		case 3:  return PF_ASTC_6x6;
		default: return PF_ASTC_4x4;
		}
	}

	/** The pixel format the engine's own encoder would pick for this texture. */
	EPixelFormat GetPixelFormat(const FFormat& Format, const FTextureBuildSettings& BuildSettings, bool bImageHasAlpha, const FConfig& Config)
	{
		switch (Format.BlockSize)
		{
		case EBlockSize::Fixed:
		case EBlockSize::ASTCNormal:
			return bImageHasAlpha ? Format.WithAlpha : Format.Opaque;
		case EBlockSize::ASTC:
		case EBlockSize::ASTCHQ:
			if (BuildSettings.bVirtualStreamable)
			{
				return PF_ASTC_4x4;
			}
			// CompressionQuality is the texture's override, 0-4, or -1 for the project default.
			if (BuildSettings.CompressionQuality >= 0)
			{
				return ASTCForQuality(BuildSettings.CompressionQuality);
			}
			return ASTCForQuality(Format.BlockSize == EBlockSize::ASTCHQ ? Config.ASTCQualityBySizeHQ : Config.ASTCQualityBySize);
		case EBlockSize::ASTCHDR:
			// The engine would pick 8x8 to 12x12 HDR blocks at qualities 0-2; Basis encodes 6x6 there.
			if (BuildSettings.bVirtualStreamable)
			{
				return PF_ASTC_4x4_HDR;
			}
			return (BuildSettings.CompressionQuality >= 0 ? BuildSettings.CompressionQuality : Config.ASTCQualityBySize) >= 4 ? PF_ASTC_4x4_HDR : PF_ASTC_6x6_HDR;
		}
		return PF_Unknown;
	}

	/**
	 * Whether a payload can stand in for this texture's mips. Only plain 2D textures uploaded
	 * through the RHI: virtual texture tiles and CPU-readable textures are read by code that
	 * expects native blocks, and arrays, cubes and volumes have several slices per mip.
	 */
	bool CanCarryPayload(const FTextureBuildSettings& BuildSettings)
	{
		return !BuildSettings.bVirtualStreamable && !BuildSettings.bCPUAccessible
			&& !BuildSettings.bCubemap && !BuildSettings.bTextureArray && !BuildSettings.bVolume;
	}

	class FTextureFormatBasis final : public ITextureFormat
	{
	public:
		explicit FTextureFormatBasis(bool bInPayload)
			: bPayload(bInPayload)
			, Prefix(bInPayload ? TEXT("TFBP_") : TEXT("TFB_"))
			, GlobalConfig(FConfig::FromEngineConfig())
		{
			for (const FFormat& Format : GFormats)
			{
				if (GlobalConfig.bEncodeHDR || !IsHDR(Format))
				{
					SupportedFormats.Add(FName(Prefix + Format.Name));
				}
			}

			if (bPayload)
			{
				static const TCHAR* const CodingNames[] = { TEXT("zstd"), TEXT("hybrid"), TEXT("arithmetic") };
				UE_LOG(LogTextureFormatBasis, Display,
					TEXT("Basis payloads (basis_universal %hs): quality %d, XUASTC %dx%d intermediate (%s)%s, effort %d"),
					BasisEncoder::LibraryVersion(), GlobalConfig.PayloadQuality, GlobalConfig.PayloadBlockWidth,
					GlobalConfig.PayloadBlockHeight, CodingNames[GlobalConfig.PayloadCoding],
					GlobalConfig.bBC7PayloadAsXUBC7 ? TEXT(", XUBC7 for BC7") : TEXT(""), GlobalConfig.Effort);
			}
		}

		virtual bool AllowParallelBuild() const override
		{
			return true;
		}

		virtual FName GetEncoderName(FName Format) const override
		{
			static const FName NativeName(TEXT("BasisUniversal"));
			static const FName PayloadName(TEXT("BasisUniversalPayload"));
			return bPayload ? PayloadName : NativeName;
		}

		virtual bool CanAcceptNonF32Source(FName Format) const override
		{
			// Every source is converted to BGRA8 (RGBA32F for HDR) in CompressImage.
			return true;
		}

		virtual uint16 GetVersion(FName Format, const FTextureBuildSettings* BuildSettings) const override
		{
			return FormatVersion;
		}

		virtual FString GetDerivedDataKeyString(const FTextureBuildSettings& BuildSettings, int32 MipCount, const FIntVector3& Mip0Dimensions) const override
		{
			// The legacy (non-build-function) cache key. The build-function key already covers the
			// config through ExportGlobalFormatConfig.
			const FConfig Config = GetConfig(BuildSettings);
			FString Key = FString::Printf(TEXT("BASIS%hs_E%d_"), BasisEncoder::LibraryVersion(), Config.Effort);
			if (bPayload)
			{
				Key += FString::Printf(TEXT("P%d_%dx%d_%d_C%d_"), Config.PayloadQuality, Config.PayloadBlockWidth, Config.PayloadBlockHeight,
					Config.bBC7PayloadAsXUBC7 ? 1 : 0, Config.PayloadCoding);
			}
			const FFormat* Format = FindFormat(BuildSettings.BaseTextureFormatName, Prefix);
			if (Format && UsesASTCQualityDefaults(*Format))
			{
				Key += FString::Printf(TEXT("A%d_%d_"), Config.ASTCQualityBySize, Config.ASTCQualityBySizeHQ);
			}
			return Key;
		}

		virtual void GetSupportedFormats(TArray<FName>& OutFormats) const override
		{
			OutFormats.Append(SupportedFormats);
		}

		virtual FString GetAlternateTextureFormatPrefix() const override
		{
			return Prefix;
		}

		virtual FCbObject ExportGlobalFormatConfig(const FTextureBuildSettings& BuildSettings) const override
		{
			// Only what this texture's output depends on, so that changing, say, the payload quality
			// does not invalidate natively encoded textures.
			FCbWriter Writer;
			Writer.BeginObject("TextureFormatBasisSettings");
			Writer.AddInteger("Effort", GlobalConfig.Effort);
			if (bPayload)
			{
				Writer.AddInteger("PayloadQuality", GlobalConfig.PayloadQuality);
				Writer.AddInteger("PayloadBlockWidth", GlobalConfig.PayloadBlockWidth);
				Writer.AddInteger("PayloadBlockHeight", GlobalConfig.PayloadBlockHeight);
				Writer.AddBool("bBC7PayloadAsXUBC7", GlobalConfig.bBC7PayloadAsXUBC7);
				Writer.AddInteger("PayloadCoding", GlobalConfig.PayloadCoding);
			}
			const FFormat* Format = FindFormat(BuildSettings.BaseTextureFormatName, Prefix);
			if (Format && UsesASTCQualityDefaults(*Format))
			{
				Writer.AddInteger("DefaultASTCQualityBySize", GlobalConfig.ASTCQualityBySize);
				Writer.AddInteger("DefaultASTCQualityBySizeHQ", GlobalConfig.ASTCQualityBySizeHQ);
			}
			Writer.EndObject();
			return Writer.Save().AsObject();
		}

		virtual EPixelFormat GetEncodedPixelFormat(const FTextureBuildSettings& BuildSettings, bool bImageHasAlphaChannel) const override
		{
			const FFormat* Format = FindFormat(BuildSettings.BaseTextureFormatName, Prefix);
			return Format ? GetPixelFormat(*Format, BuildSettings, bImageHasAlphaChannel, GetConfig(BuildSettings)) : PF_Unknown;
		}

		virtual bool CompressImage(
			const FImage& InImage,
			const FTextureBuildSettings& BuildSettings,
			const FIntVector3& InMip0Dimensions,
			int32 InMip0NumSlicesNoDepth,
			int32 InMipIndex,
			int32 InMipCount,
			FStringView DebugTexturePathName,
			bool bImageHasAlphaChannel,
			FCompressedImage2D& OutCompressedImage) const override
		{
			const FFormat* Format = FindFormat(BuildSettings.BaseTextureFormatName, Prefix);
			if (Format == nullptr)
			{
				UE_LOG(LogTextureFormatBasis, Error, TEXT("%.*s: unsupported format %s"),
					DebugTexturePathName.Len(), DebugTexturePathName.GetData(), *BuildSettings.BaseTextureFormatName.ToString());
				return false;
			}

			const FConfig Config = GetConfig(BuildSettings);
			const EPixelFormat PixelFormat = GetPixelFormat(*Format, BuildSettings, bImageHasAlphaChannel, Config);
			const int32 TranscoderTarget = UE::BasisTranscoder::GetTranscoderTarget(PixelFormat);
			check(TranscoderTarget >= 0);

			BasisEncoder::FSettings Settings;
			Settings.TranscoderTarget = TranscoderTarget;
			Settings.Layout = Format->Layout;
			Settings.bHasAlpha = Format->bKeepsAlpha && bImageHasAlphaChannel;
			Settings.bSRGB = BuildSettings.GetDestGammaSpace() == EGammaSpace::sRGB;
			Settings.Output = bPayload && CanCarryPayload(BuildSettings) ? BasisEncoder::EOutput::Payload : BasisEncoder::EOutput::Native;
			Settings.Quality = Config.PayloadQuality;
			Settings.Effort = Config.Effort;
			Settings.PayloadBlockWidth = (uint32)Config.PayloadBlockWidth;
			Settings.PayloadBlockHeight = (uint32)Config.PayloadBlockHeight;
			Settings.bBC7PayloadAsXUBC7 = Config.bBC7PayloadAsXUBC7;
			Settings.PayloadCoding = (BasisEncoder::EPayloadCoding)Config.PayloadCoding;

			// The encoder works on 8-bit BGRA in the destination's colour space, or linear RGBA32F for HDR.
			FImage Image;
			BasisEncoder::FResult Result;
			bool bEncoded;
			if (IsHDR(*Format))
			{
				InImage.CopyTo(Image, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
				bEncoded = BasisEncoder::EncodeHDR((const float*)Image.RawData.GetData(), (uint32)Image.SizeX, (uint32)Image.SizeY, (uint32)Image.NumSlices, Settings, Result);
			}
			else
			{
				InImage.CopyTo(Image, ERawImageFormat::BGRA8, BuildSettings.GetDestGammaSpace());
				bEncoded = BasisEncoder::Encode(Image.RawData.GetData(), (uint32)Image.SizeX, (uint32)Image.SizeY, (uint32)Image.NumSlices, Settings, Result);
			}
			if (!bEncoded)
			{
				UE_LOG(LogTextureFormatBasis, Error, TEXT("%.*s: mip %d (%dx%dx%d) to %s failed: %hs"),
					DebugTexturePathName.Len(), DebugTexturePathName.GetData(), InMipIndex, Image.SizeX, Image.SizeY, Image.NumSlices,
					GetPixelFormatString(PixelFormat), Result.Error.c_str());
				return false;
			}
			if (!Result.Note.empty())
			{
				// Expected for the smallest mips, whose payload would outweigh their blocks.
				UE_LOG(LogTextureFormatBasis, Verbose, TEXT("%.*s: mip %d (%dx%d): %hs"),
					DebugTexturePathName.Len(), DebugTexturePathName.GetData(), InMipIndex, Image.SizeX, Image.SizeY, Result.Note.c_str());
			}

			// A payload sits at the start of a mip of the native size, zero-filled after it; native
			// blocks fill it exactly.
			const int64 NativeSize = (int64)BasisEncoder::NativeSliceSize(TranscoderTarget, (uint32)Image.SizeX, (uint32)Image.SizeY) * Image.NumSlices;
			check((int64)Result.Data.size() <= NativeSize && (Result.bPayload || (int64)Result.Data.size() == NativeSize));
			OutCompressedImage.RawData.SetNumZeroed(NativeSize);
			FMemory::Memcpy(OutCompressedImage.RawData.GetData(), Result.Data.data(), Result.Data.size());
			OutCompressedImage.SizeX = Image.SizeX;
			OutCompressedImage.SizeY = Image.SizeY;
			OutCompressedImage.NumSlicesWithDepth = Image.NumSlices;
			OutCompressedImage.PixelFormat = PixelFormat;
			return true;
		}

		// Decoding serves the editor's platform preview and the derived-data decode build function.
		static FGuid GetDecodeBuildFunctionVersionGuid()
		{
			static const FGuid Version(TEXT("d4bed278-d617-4550-8342-aae7f8882ce2"));
			return Version;
		}

		static FUtf8StringView GetDecodeBuildFunctionNameStatic()
		{
			return UTF8TEXTVIEW("FDecodeTextureFormatBasis");
		}

		virtual const FUtf8StringView GetDecodeBuildFunctionName() const override final
		{
			return GetDecodeBuildFunctionNameStatic();
		}

		virtual bool CanDecodeFormat(EPixelFormat InPixelFormat) const override
		{
			return UE::BasisTranscoder::GetTranscoderTarget(InPixelFormat) >= 0;
		}

		virtual bool DecodeImage(int32 InSizeX, int32 InSizeY, int32 InNumSlices, EPixelFormat InPixelFormat, bool bInSRGB,
			const FName& InTextureFormatName, FSharedBuffer InEncodedData, FImage& OutImage, FStringView InTextureName) const override
		{
			// As the engine's decoders return them: BGRA8 in the texture's gamma, or linear RGBA16F
			// with alpha 1 for HDR. The ASTC normal-map layouts come back as (X, Y, 0, 1).
			const int32 TranscoderTarget = UE::BasisTranscoder::GetTranscoderTarget(InPixelFormat);
			if (TranscoderTarget < 0 || InSizeX <= 0 || InSizeY <= 0 || InNumSlices <= 0)
			{
				return false;
			}
			const FFormat* Format = FindFormat(InTextureFormatName, Prefix);
			const uint8* Data = (const uint8*)InEncodedData.GetData();
			bool bDecoded;
			if (BasisEncoder::IsHDRTarget(TranscoderTarget))
			{
				FImage Decoded(InSizeX, InSizeY, InNumSlices, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
				bDecoded = BasisEncoder::DecodeHDR(Data, InEncodedData.GetSize(), TranscoderTarget, (uint32)InSizeX, (uint32)InSizeY,
					(uint32)InNumSlices, (float*)Decoded.RawData.GetData());
				if (bDecoded)
				{
					Decoded.CopyTo(OutImage, ERawImageFormat::RGBA16F, EGammaSpace::Linear);
				}
			}
			else
			{
				OutImage.Init(InSizeX, InSizeY, InNumSlices, ERawImageFormat::BGRA8, bInSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
				bDecoded = BasisEncoder::Decode(Data, InEncodedData.GetSize(), TranscoderTarget, (uint32)InSizeX, (uint32)InSizeY,
					(uint32)InNumSlices, Format ? Format->Layout : EChannelLayout::RGBA, bInSRGB, OutImage.RawData.GetData());
			}
			if (!bDecoded)
			{
				UE_LOG(LogTextureFormatBasis, Warning, TEXT("%.*s: could not decode %dx%dx%d %s from %llu bytes"),
					InTextureName.Len(), InTextureName.GetData(), InSizeX, InSizeY, InNumSlices, GetPixelFormatString(InPixelFormat),
					(unsigned long long)InEncodedData.GetSize());
			}
			return bDecoded;
		}

	private:
		FConfig GetConfig(const FTextureBuildSettings& BuildSettings) const
		{
			return BuildSettings.FormatConfigOverride ? FConfig::FromFormatConfig(BuildSettings.FormatConfigOverride, GlobalConfig) : GlobalConfig;
		}

		const bool bPayload;
		const FString Prefix;
		const FConfig GlobalConfig;
		TArray<FName> SupportedFormats;
	};

	/** The cache build function, named by the engine's convention from the module's name. */
	class FBasisTextureBuildFunction final : public FTextureBuildFunction
	{
		virtual const UE::FUtf8SharedString& GetName() const override
		{
			static const UE::FUtf8SharedString Name(UTF8TEXTVIEW("BasisTexture"));
			return Name;
		}

		virtual void GetVersion(UE::DerivedData::FBuildVersionBuilder& Builder, ITextureFormat*& OutTextureFormatVersioning) const override
		{
			static const FGuid Version(TEXT("58de4155-5fb7-4fb6-8d99-239e464f498b"));
			Builder << Version;
			UE::TextureFormatBasis::AddBuildVersion(Builder);
			OutTextureFormatVersioning = FModuleManager::GetModuleChecked<ITextureFormatModule>(TEXT("TextureFormatBasis")).GetTextureFormat();
		}
	};
}

TUniquePtr<ITextureFormat> UE::TextureFormatBasis::CreateTextureFormat(bool bPayload)
{
	return MakeUnique<Private::FTextureFormatBasis>(bPayload);
}

void UE::TextureFormatBasis::AddBuildVersion(UE::DerivedData::FBuildVersionBuilder& Builder)
{
	Builder << FString(BasisEncoder::LibraryVersion());
}

/** Encodes to native blocks: a platform selects it with the TFB_ prefix. */
class FTextureFormatBasisModule final : public ITextureFormatModule
{
public:
	virtual void StartupModule() override
	{
		TextureFormat = UE::TextureFormatBasis::CreateTextureFormat(false);
	}

	virtual void ShutdownModule() override
	{
		TextureFormat.Reset();
	}

	virtual bool CanCallGetTextureFormats() override
	{
		return false;
	}

	virtual ITextureFormat* GetTextureFormat() override
	{
		return TextureFormat.Get();
	}

private:
	TUniquePtr<ITextureFormat> TextureFormat;

	static inline UE::DerivedData::TBuildFunctionFactory<UE::TextureFormatBasis::Private::FBasisTextureBuildFunction> BuildFunctionFactory;
	// One decode function serves both modules' formats: it finds the format by name.
	static inline UE::DerivedData::TBuildFunctionFactory<FGenericTextureDecodeBuildFunction<UE::TextureFormatBasis::Private::FTextureFormatBasis>> DecodeBuildFunctionFactory;
};

IMPLEMENT_MODULE(FTextureFormatBasisModule, TextureFormatBasis)
