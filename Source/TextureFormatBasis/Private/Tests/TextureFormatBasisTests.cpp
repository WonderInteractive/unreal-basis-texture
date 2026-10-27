// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

// Editor automation tests for the Unreal side of the plugin, through the interfaces the engine calls:
// the texture format manager, CompressImage, DecodeImage and IBasisTranscoder. Run them from the
// Session Frontend or with -ExecCmds="Automation RunTests Plugins.TextureFormatBasis". The encoder
// and transcoder cores have their own standalone tests (Tests/ in the plugin).

#include "CoreTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BasisTranscoder.h"
#include "ImageCore.h"
#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatManagerModule.h"
#include "Memory/SharedBuffer.h"
#include "Misc/ConfigCacheIni.h"
#include "TextureCompressorModule.h"
#include "TextureFormatManager.h"

namespace UE::TextureFormatBasis::Tests
{
	constexpr int32 Width = 67;
	constexpr int32 Height = 45;

	// The engine format names the plugin takes (TextureFormatBasis.cpp, GFormats).
	const TCHAR* const LDRFormats[] =
	{
		TEXT("DXT1"), TEXT("DXT5"), TEXT("AutoDXT"), TEXT("DXT5n"), TEXT("BC4"), TEXT("BC5"), TEXT("BC7"),
		TEXT("ETC2_RGB"), TEXT("ETC2_RGBA"), TEXT("AutoETC2"), TEXT("ETC2_R11"), TEXT("ETC2_RG11"),
		TEXT("ASTC_RGB"), TEXT("ASTC_RGBA"), TEXT("ASTC_RGBAuto"), TEXT("ASTC_RGBA_HQ"),
		TEXT("ASTC_NormalAG"), TEXT("ASTC_NormalRG"), TEXT("ASTC_NormalLA"), TEXT("ASTC_NormalRG_Precise"),
	};
	const TCHAR* const HDRFormats[] = { TEXT("BC6H"), TEXT("ASTC_RGB_HDR") };

	bool EncodesHDR()
	{
		bool bEncodeHDR = true;
		GConfig->GetBool(TEXT("TextureFormatBasisSettings"), TEXT("bEncodeHDR"), bEncodeHDR, GEngineIni);
		return bEncodeHDR;
	}

	const ITextureFormat* FindFormat(FAutomationTestBase& Test, const FString& Name)
	{
		ITextureFormatManagerModule* Manager = GetTextureFormatManager();
		const ITextureFormat* Format = Manager ? Manager->FindTextureFormat(FName(Name)) : nullptr;
		if (Format == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("the texture format manager has no %s"), *Name));
		}
		return Format;
	}

	/** Smooth colour and alpha with some detail, at odd dimensions so that edge blocks are partial. */
	FImage MakeImage(int32 SizeX, int32 SizeY, EGammaSpace GammaSpace)
	{
		FImage Image(SizeX, SizeY, ERawImageFormat::BGRA8, GammaSpace);
		TArrayView64<FColor> Pixels = Image.AsBGRA8();
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const float U = (float)X / SizeX, V = (float)Y / SizeY;
				FColor& P = Pixels[(int64)Y * SizeX + X];
				P.R = (uint8)(255.f * U);
				P.G = (uint8)(255.f * V);
				P.B = (uint8)(127.5f + 127.f * FMath::Sin(6.f * U + 4.f * V));
				P.A = (uint8)(255.f * (1.f - 0.75f * U * V));
			}
		}
		return Image;
	}

	/** Linear HDR from 1/8 to about 128, with an alpha the encoders must ignore. */
	FImage MakeHDRImage(int32 SizeX, int32 SizeY)
	{
		FImage Image(SizeX, SizeY, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		TArrayView64<FLinearColor> Pixels = Image.AsRGBA32F();
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const float U = (float)X / SizeX, V = (float)Y / SizeY;
				Pixels[(int64)Y * SizeX + X] = FLinearColor(FMath::Exp2(10.f * U - 3.f), 4.f + 3.5f * FMath::Sin(6.f * U + 4.f * V), FMath::Exp2(9.f * U * V - 2.f), 0.25f);
			}
		}
		return Image;
	}

	uint8 Channel(const FColor& Color, int32 Index)
	{
		return Index == 0 ? Color.R : Index == 1 ? Color.G : Index == 2 ? Color.B : Color.A;
	}

	FTextureBuildSettings MakeBuildSettings(const FString& Name, bool bSRGB)
	{
		FTextureBuildSettings Settings;
		Settings.BaseTextureFormatName = FName(Name);
		Settings.TextureFormatName = FName(Name);
		Settings.bSRGB = bSRGB;
		return Settings;
	}

	bool Compress(FAutomationTestBase& Test, const ITextureFormat& Format, const FImage& Source, const FTextureBuildSettings& Settings, FCompressedImage2D& Out)
	{
		FImage Copy = Source; // CompressImage may consume its input
		if (!Format.CompressImage(Copy, Settings, FIntVector3(Source.SizeX, Source.SizeY, 1), 1, 0, 1, TEXT("TextureFormatBasisTests"), true, Out))
		{
			Test.AddError(FString::Printf(TEXT("%s: CompressImage failed"), *Settings.BaseTextureFormatName.ToString()));
			return false;
		}
		return true;
	}

	bool Decode(FAutomationTestBase& Test, const ITextureFormat& Format, const FCompressedImage2D& Compressed, const FTextureBuildSettings& Settings, FImage& Out)
	{
		const FString Name = Settings.BaseTextureFormatName.ToString();
		if (!Format.CanDecodeFormat(Compressed.PixelFormat))
		{
			Test.AddError(FString::Printf(TEXT("%s: cannot decode %s"), *Name, GetPixelFormatString(Compressed.PixelFormat)));
			return false;
		}
		if (!Format.DecodeImage(Compressed.SizeX, Compressed.SizeY, Compressed.NumSlicesWithDepth, Compressed.PixelFormat, Settings.bSRGB,
			Settings.BaseTextureFormatName, FSharedBuffer::Clone(Compressed.RawData.GetData(), Compressed.RawData.Num()), Out, TEXT("TextureFormatBasisTests")))
		{
			Test.AddError(FString::Printf(TEXT("%s: DecodeImage failed"), *Name));
			return false;
		}
		return true;
	}
}

using namespace UE::TextureFormatBasis::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureFormatBasisRegistrationTest, "Plugins.TextureFormatBasis.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTextureFormatBasisRegistrationTest::RunTest(const FString& Parameters)
{
	// Every name under both prefixes reaches the plugin's encoders.
	const bool bHDR = EncodesHDR();
	for (const TCHAR* Prefix : { TEXT("TFB_"), TEXT("TFBP_") })
	{
		const FName EncoderName(FCString::Strcmp(Prefix, TEXT("TFB_")) == 0 ? TEXT("BasisUniversal") : TEXT("BasisUniversalPayload"));
		TArray<const TCHAR*> Names(LDRFormats, UE_ARRAY_COUNT(LDRFormats));
		if (bHDR)
		{
			Names.Append(HDRFormats, UE_ARRAY_COUNT(HDRFormats));
		}
		for (const TCHAR* Name : Names)
		{
			const FString Prefixed = FString(Prefix) + Name;
			if (const ITextureFormat* Format = FindFormat(*this, Prefixed))
			{
				TestEqual(Prefixed + TEXT(" encoder"), Format->GetEncoderName(FName(Prefixed)), EncoderName);
				TestEqual(Prefixed + TEXT(" prefix"), Format->GetAlternateTextureFormatPrefix(), FString(Prefix));
			}
		}
	}

	// The runtime transcoder loads with the plugin, and refuses data that is not a payload.
	IBasisTranscoder* Transcoder = IBasisTranscoder::Get();
	if (TestNotNull(TEXT("IBasisTranscoder::Get()"), Transcoder))
	{
		TArray<uint8> Zeros;
		Zeros.SetNumZeroed(4096);
		IBasisTranscoder::FPayloadInfo Info;
		TestFalse(TEXT("GetPayloadInfo on zeros"), Transcoder->GetPayloadInfo(MakeMemoryView(Zeros), Info));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureFormatBasisRoundTripTest, "Plugins.TextureFormatBasis.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTextureFormatBasisRoundTripTest::RunTest(const FString& Parameters)
{
	// Encode with CompressImage and decode with DecodeImage, as the editor does for a platform
	// preview, and compare the channels each format carries with the source. The floors are well
	// below what the encoders reach; they catch wrong formats, layouts and swizzles.
	struct FCase
	{
		const TCHAR* Name;
		/** Expected pixel format, or PF_Unknown for an ASTC format whose block size is configurable. */
		EPixelFormat PixelFormat;
		/** (decoded channel, source channel) pairs, as RGBA indices. */
		TArray<FIntPoint> Channels;
		double MinPsnr;
		bool bSRGB = false;
	};
	const TArray<FIntPoint> RGB = { { 0, 0 }, { 1, 1 }, { 2, 2 } };
	const TArray<FIntPoint> RGBA = { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 } };
	const TArray<FIntPoint> R = { { 0, 0 } };
	const TArray<FIntPoint> RG = { { 0, 0 }, { 1, 1 } };
	const TArray<FCase> Cases =
	{
		{ TEXT("DXT1"),                  PF_DXT1,             RGB,  30 },
		{ TEXT("DXT5"),                  PF_DXT5,             RGBA, 30 },
		{ TEXT("AutoDXT"),               PF_DXT5,             RGBA, 30 },
		{ TEXT("DXT5n"),                 PF_DXT5,             { { 3, 0 }, { 1, 1 } }, 32 }, // X in alpha, undecoded as Oodle does
		{ TEXT("BC4"),                   PF_BC4,              R,    36 },
		{ TEXT("BC5"),                   PF_BC5,              RG,   36 },
		{ TEXT("BC7"),                   PF_BC7,              RGBA, 32 },
		{ TEXT("BC7"),                   PF_BC7,              RGBA, 32, true },
		{ TEXT("ETC2_RGB"),              PF_ETC2_RGB,         RGB,  28 },
		{ TEXT("ETC2_RGBA"),             PF_ETC2_RGBA,        RGBA, 28 },
		{ TEXT("AutoETC2"),              PF_ETC2_RGBA,        RGBA, 28 },
		{ TEXT("ETC2_R11"),              PF_ETC2_R11_EAC,     R,    36 },
		{ TEXT("ETC2_RG11"),             PF_ETC2_RG11_EAC,    RG,   36 },
		{ TEXT("ASTC_RGB"),              PF_Unknown,          RGB,  26 },
		{ TEXT("ASTC_RGBA"),             PF_Unknown,          RGBA, 26 },
		{ TEXT("ASTC_RGBAuto"),          PF_Unknown,          RGBA, 26 },
		{ TEXT("ASTC_RGBA_HQ"),          PF_Unknown,          RGBA, 26 },
		{ TEXT("ASTC_NormalAG"),         PF_ASTC_6x6,         RG,   32 },
		{ TEXT("ASTC_NormalRG"),         PF_ASTC_6x6,         RG,   32 },
		{ TEXT("ASTC_NormalLA"),         PF_ASTC_6x6,         RG,   32 },
		{ TEXT("ASTC_NormalRG_Precise"), PF_ASTC_6x6_NORM_RG, RG,   32 },
	};

	for (const FCase& Case : Cases)
	{
		const FString Name = FString(TEXT("TFB_")) + Case.Name;
		const FString Label = Name + (Case.bSRGB ? TEXT(" sRGB") : TEXT(""));
		const ITextureFormat* Format = FindFormat(*this, Name);
		if (Format == nullptr)
		{
			continue;
		}
		const FTextureBuildSettings Settings = MakeBuildSettings(Name, Case.bSRGB);
		const FImage Source = MakeImage(Width, Height, Settings.GetDestGammaSpace());
		FCompressedImage2D Compressed;
		FImage Decoded;
		if (!Compress(*this, *Format, Source, Settings, Compressed) || !Decode(*this, *Format, Compressed, Settings, Decoded))
		{
			continue;
		}

		const EPixelFormat Expected = Case.PixelFormat != PF_Unknown ? Case.PixelFormat : Format->GetEncodedPixelFormat(Settings, true);
		TestEqual(Label + TEXT(" pixel format"), (int32)Compressed.PixelFormat, (int32)Expected);
		TestTrue(Label + TEXT(" decodes to BGRA8"), Decoded.Format == ERawImageFormat::BGRA8 && Decoded.SizeX == Width && Decoded.SizeY == Height);
		if (Decoded.Format != ERawImageFormat::BGRA8 || Decoded.SizeX != Width || Decoded.SizeY != Height)
		{
			continue;
		}
		TestEqual(Label + TEXT(" decoded gamma"), (int32)Decoded.GammaSpace, (int32)(Case.bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear));

		const TArrayView64<const FColor> In = Source.AsBGRA8();
		const TArrayView64<FColor> Out = Decoded.AsBGRA8();
		double Sum = 0;
		for (int64 I = 0; I < In.Num(); ++I)
		{
			for (const FIntPoint& Pair : Case.Channels)
			{
				const double E = (double)Channel(Out[I], Pair.X) - (double)Channel(In[I], Pair.Y);
				Sum += E * E;
			}
		}
		const double Mse = Sum / (double)(In.Num() * Case.Channels.Num());
		const double Psnr = Mse > 0 ? 10.0 * FMath::LogX(10.0, 255.0 * 255.0 / Mse) : 99.0;
		AddInfo(FString::Printf(TEXT("%s: %s, %.1f dB"), *Label, GetPixelFormatString(Compressed.PixelFormat), Psnr));
		TestTrue(FString::Printf(TEXT("%s: %.1f dB is at least %.0f dB"), *Label, Psnr, Case.MinPsnr), Psnr >= Case.MinPsnr);
	}

	if (!EncodesHDR())
	{
		AddInfo(TEXT("bEncodeHDR is off: HDR formats skipped"));
		return true;
	}
	const FImage HDRSource = MakeHDRImage(Width, Height);
	for (const TCHAR* HDRName : HDRFormats)
	{
		const FString Name = FString(TEXT("TFB_")) + HDRName;
		const ITextureFormat* Format = FindFormat(*this, Name);
		if (Format == nullptr)
		{
			continue;
		}
		const FTextureBuildSettings Settings = MakeBuildSettings(Name, false);
		FCompressedImage2D Compressed;
		FImage Decoded;
		if (!Compress(*this, *Format, HDRSource, Settings, Compressed) || !Decode(*this, *Format, Compressed, Settings, Decoded))
		{
			continue;
		}
		TestEqual(Name + TEXT(" pixel format"), (int32)Compressed.PixelFormat, (int32)Format->GetEncodedPixelFormat(Settings, true));
		if (!TestTrue(Name + TEXT(" decodes to RGBA16F"), Decoded.Format == ERawImageFormat::RGBA16F && Decoded.SizeX == Width && Decoded.SizeY == Height))
		{
			continue;
		}
		FImage Decoded32;
		Decoded.CopyTo(Decoded32, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		const TArrayView64<const FLinearColor> In = HDRSource.AsRGBA32F();
		const TArrayView64<FLinearColor> Out = Decoded32.AsRGBA32F();
		double Sum = 0;
		bool bOpaque = true;
		for (int64 I = 0; I < In.Num(); ++I)
		{
			for (int32 C = 0; C < 3; ++C)
			{
				const double E = FMath::Log2(1.0 + Out[I].Component(C)) - FMath::Log2(1.0 + In[I].Component(C));
				Sum += E * E;
			}
			bOpaque &= Out[I].A == 1.f;
		}
		const double Rmse = FMath::Sqrt(Sum / (double)(In.Num() * 3));
		AddInfo(FString::Printf(TEXT("%s: %s, log2 RMSE %.4f"), *Name, GetPixelFormatString(Compressed.PixelFormat), Rmse));
		TestTrue(FString::Printf(TEXT("%s: log2 RMSE %.4f is at most 0.1"), *Name, Rmse), Rmse <= 0.1);
		TestTrue(Name + TEXT(" decodes opaque"), bOpaque);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureFormatBasisPayloadTest, "Plugins.TextureFormatBasis.Payload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTextureFormatBasisPayloadTest::RunTest(const FString& Parameters)
{
	// A payload format writes a payload the runtime transcoder turns into the native blocks that
	// decode to the same image, and native blocks where a payload cannot stand in.
	IBasisTranscoder* Transcoder = IBasisTranscoder::Get();
	if (!TestNotNull(TEXT("IBasisTranscoder::Get()"), Transcoder))
	{
		return false;
	}

	TArray<const TCHAR*> Names = { TEXT("BC7"), TEXT("DXT5"), TEXT("ETC2_RGBA"), TEXT("ASTC_RGBA") };
	if (EncodesHDR())
	{
		Names.Add(TEXT("BC6H"));
	}
	const FImage LDRSource = MakeImage(256, 256, EGammaSpace::Linear);
	const FImage HDRSource = MakeHDRImage(256, 256);
	for (const TCHAR* BaseName : Names)
	{
		const FString Name = FString(TEXT("TFBP_")) + BaseName;
		const ITextureFormat* Format = FindFormat(*this, Name);
		if (Format == nullptr)
		{
			continue;
		}
		const bool bHDR = FCString::Strcmp(BaseName, TEXT("BC6H")) == 0;
		const FImage& Source = bHDR ? HDRSource : LDRSource;
		const FTextureBuildSettings Settings = MakeBuildSettings(Name, false);
		FCompressedImage2D Payload;
		if (!Compress(*this, *Format, Source, Settings, Payload))
		{
			continue;
		}

		IBasisTranscoder::FPayloadInfo Info;
		const FMemoryView Mip = MakeMemoryView(Payload.RawData.GetData(), (uint64)Payload.RawData.Num());
		if (!TestTrue(Name + TEXT(" wrote a payload"), Transcoder->GetPayloadInfo(Mip, Info)))
		{
			continue;
		}
		TestTrue(Name + TEXT(" payload dimensions"), Info.Width == 256 && Info.Height == 256);
		AddInfo(FString::Printf(TEXT("%s: %u-byte payload for %lld native bytes"), *Name, Info.PayloadSize, (long long)Payload.RawData.Num()));
		TestTrue(Name + TEXT(" CanTranscode"), Transcoder->CanTranscode(Mip, Payload.PixelFormat));

		FCompressedImage2D Native;
		Native.RawData.SetNumZeroed(Payload.RawData.Num());
		Native.SizeX = Payload.SizeX;
		Native.SizeY = Payload.SizeY;
		Native.NumSlicesWithDepth = Payload.NumSlicesWithDepth;
		Native.PixelFormat = Payload.PixelFormat;
		if (!TestTrue(Name + TEXT(" Transcode"), Transcoder->Transcode(Mip, Payload.PixelFormat, MakeMemoryView(Native.RawData.GetData(), (uint64)Native.RawData.Num()))))
		{
			continue;
		}
		FImage FromPayload, FromNative;
		if (Decode(*this, *Format, Payload, Settings, FromPayload) && Decode(*this, *Format, Native, Settings, FromNative))
		{
			TestTrue(Name + TEXT(": the payload decodes as its transcode does"), FromPayload.RawData == FromNative.RawData);
		}
	}

	// Virtual texture tiles are read by code other than the RHI, so they stay native.
	if (const ITextureFormat* Format = FindFormat(*this, TEXT("TFBP_BC7")))
	{
		FTextureBuildSettings Settings = MakeBuildSettings(TEXT("TFBP_BC7"), false);
		Settings.bVirtualStreamable = true;
		FCompressedImage2D Compressed;
		if (Compress(*this, *Format, LDRSource, Settings, Compressed))
		{
			IBasisTranscoder::FPayloadInfo Info;
			TestFalse(TEXT("virtual texture tile is native"), Transcoder->GetPayloadInfo(MakeMemoryView(Compressed.RawData.GetData(), (uint64)Compressed.RawData.Num()), Info));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
