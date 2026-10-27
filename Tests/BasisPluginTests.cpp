// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

// Tests for the plugin's Unreal-free cores. Every texture is decoded back with basis_universal's
// own block unpackers and compared with what the destination format should hold, channel by
// channel, so a wrong layout or a wrong block size fails here rather than on a device.

#include "BasisEncoder.h"
#include "BasisTranscodeCore.h"

#include "BasisUniversalBegin.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_gpu_texture.h"
#include "transcoder/basisu_transcoder.h"
#include "BasisUniversalEnd.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using basist::transcoder_texture_format;
using BasisEncoder::EChannelLayout;
using BasisEncoder::EOutput;
using BasisEncoder::FResult;
using BasisEncoder::FSettings;

namespace
{
	int GFailures = 0;

	void Fail(const std::string& Message)
	{
		std::printf("FAIL: %s\n", Message.c_str());
		++GFailures;
	}

	void Check(bool bCondition, const std::string& Message)
	{
		if (!bCondition)
		{
			Fail(Message);
		}
	}

	/** Where TestPayloads writes fixtures for basis_runtime_tests, if anywhere. */
	std::string GFixtureDir;

	/**
	 * A payload in its native-sized buffer and what the full library made of it. RuntimeTests.cpp
	 * reads these: uint32 target, uint32 buffer size, uint32 native size, buffer, native blocks.
	 */
	void WriteFixture(const std::string& Name, int32_t Target, const std::vector<uint8_t>& Buffer, const std::vector<uint8_t>& Native)
	{
		std::ofstream File(GFixtureDir + "/" + Name + ".fixture", std::ios::binary);
		const uint32_t Header[3] = { (uint32_t)Target, (uint32_t)Buffer.size(), (uint32_t)Native.size() };
		File.write(reinterpret_cast<const char*>(Header), sizeof(Header));
		File.write(reinterpret_cast<const char*>(Buffer.data()), (std::streamsize)Buffer.size());
		File.write(reinterpret_cast<const char*>(Native.data()), (std::streamsize)Native.size());
		if (!File)
		{
			Fail("could not write fixture " + Name);
		}
	}

	struct FImage
	{
		uint32_t Width = 0, Height = 0, NumSlices = 1;
		std::vector<uint8_t> BGRA; // tightly packed, slice after slice

		uint8_t At(uint32_t Slice, uint32_t X, uint32_t Y, int Channel /*0=R 1=G 2=B 3=A*/) const
		{
			static const int BgraIndex[4] = { 2, 1, 0, 3 };
			return BGRA[(((uint64_t)Slice * Height + Y) * Width + X) * 4 + BgraIndex[Channel]];
		}
	};

	/** Smooth, deterministic content with some texture; each slice and channel distinct. */
	FImage MakeImage(uint32_t Width, uint32_t Height, uint32_t NumSlices = 1, bool bAlpha = true)
	{
		FImage Image;
		Image.Width = Width;
		Image.Height = Height;
		Image.NumSlices = NumSlices;
		Image.BGRA.resize((size_t)Width * Height * NumSlices * 4);
		uint32_t Seed = 12345;
		auto Noise = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return (int)((Seed >> 24) % 9) - 4; };
		auto Clamp = [](double V) { return (uint8_t)(V < 0 ? 0 : (V > 255 ? 255 : V)); };
		for (uint32_t S = 0; S < NumSlices; ++S)
		{
			for (uint32_t Y = 0; Y < Height; ++Y)
			{
				for (uint32_t X = 0; X < Width; ++X)
				{
					const double U = (double)X / Width, V = (double)Y / Height;
					const double R = 255 * U + Noise();
					const double G = 255 * V + Noise();
					const double B = 128 + 100 * std::sin(6.2831 * (U + V + 0.2 * S)) + Noise();
					const double A = bAlpha ? 255 * (0.5 + 0.5 * std::cos(6.2831 * U * 2)) : 255;
					uint8_t* P = &Image.BGRA[(((size_t)S * Height + Y) * Width + X) * 4];
					P[0] = Clamp(B); P[1] = Clamp(G); P[2] = Clamp(R); P[3] = Clamp(A);
				}
			}
		}
		return Image;
	}

	/** Expected destination value of a channel, given the encoder's channel layout. */
	int Expected(const FImage& Image, EChannelLayout Layout, bool bHasAlpha, uint32_t S, uint32_t X, uint32_t Y, int Channel)
	{
		const int R = Image.At(S, X, Y, 0), G = Image.At(S, X, Y, 1), B = Image.At(S, X, Y, 2), A = Image.At(S, X, Y, 3);
		switch (Layout)
		{
		case EChannelLayout::RGBA: { const int V[4] = { R, G, B, bHasAlpha ? A : 255 }; return V[Channel]; }
		case EChannelLayout::RRRG: { const int V[4] = { R, R, R, G }; return V[Channel]; }
		case EChannelLayout::RRR1: { const int V[4] = { R, R, R, 255 }; return V[Channel]; }
		case EChannelLayout::RG01: { const int V[4] = { R, G, 0, 255 }; return V[Channel]; }
		case EChannelLayout::AGR:  { const int V[4] = { 255, G, 0, R }; return V[Channel]; }
		}
		return 0;
	}

	double Psnr(double SumSquaredError, uint64_t Count)
	{
		if (Count == 0) return 0;
		const double Mse = SumSquaredError / (double)Count;
		return Mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / Mse);
	}

	/**
	 * Decode one slice of native blocks and return the PSNR over Channels (indices into the
	 * DECODED RGBA). ExpectedChannel maps each decoded channel to the source expectation.
	 */
	double SlicePsnr(const uint8_t* Blocks, transcoder_texture_format Target, const FImage& Image, uint32_t Slice,
		EChannelLayout Layout, bool bHasAlpha, const std::vector<int>& Channels, bool bSRGB = false)
	{
		basisu::gpu_image Gpu;
		Gpu.init(basist::basis_get_basisu_texture_format(Target), Image.Width, Image.Height);
		std::memcpy(Gpu.get_ptr(), Blocks, Gpu.get_size_in_bytes());
		basisu::image Decoded;
		if (!Gpu.unpack(Decoded, bSRGB))
		{
			Fail(std::string("unpack failed for ") + basist::basis_get_format_name(Target));
			return 0;
		}
		double Sum = 0;
		uint64_t Count = 0;
		for (uint32_t Y = 0; Y < Image.Height; ++Y)
		{
			for (uint32_t X = 0; X < Image.Width; ++X)
			{
				for (int C : Channels)
				{
					const double D = (double)Decoded(X, Y)[C] - Expected(Image, Layout, bHasAlpha, Slice, X, Y, C);
					Sum += D * D;
					++Count;
				}
			}
		}
		return Psnr(Sum, Count);
	}

	bool EncodeImage(const FImage& Image, const FSettings& Settings, FResult& Out, const std::string& Label)
	{
		if (!BasisEncoder::Encode(Image.BGRA.data(), Image.Width, Image.Height, Image.NumSlices, Settings, Out))
		{
			Fail(Label + ": encode failed: " + Out.Error);
			return false;
		}
		return true;
	}

	struct FNativeCase
	{
		transcoder_texture_format Target;
		EChannelLayout Layout;
		bool bHasAlpha;
		std::vector<int> Channels; // decoded channels that carry data for this layout and target
		double MinPsnr;
	};

	void TestNativeFormats()
	{
		using T = transcoder_texture_format;
		using L = EChannelLayout;
		const std::vector<FNativeCase> Cases =
		{
			// BC1-5 and EAC are encoded directly; the bars sit about 3 dB under what they reach, well
			// above what the UASTC route gave (BC5 and RG11 G about 41 dB, R11 about 49.5 dB).
			{ T::cTFBC1_RGB,            L::RGBA, false, { 0, 1, 2 },    33 },
			{ T::cTFBC3_RGBA,           L::RGBA, true,  { 0, 1, 2, 3 }, 34 },
			{ T::cTFBC3_RGBA,           L::AGR,  true,  { 1, 3 },       42 }, // DXT5n: X in A, Y in G
			{ T::cTFBC4_R,              L::RRR1, false, { 0 },          48 },
			{ T::cTFBC5_RG,             L::RRRG, true,  { 0 },          47 }, // BC5 R = X ...
			{ T::cTFBC7_RGBA,           L::RGBA, true,  { 0, 1, 2, 3 }, 34 },
			{ T::cTFETC1_RGB,           L::RGBA, false, { 0, 1, 2 },    30 },
			{ T::cTFETC2_RGBA,          L::RGBA, true,  { 0, 1, 2, 3 }, 30 },
			{ T::cTFETC2_EAC_R11,       L::RRR1, false, { 0 },          48 },
			{ T::cTFETC2_EAC_RG11,      L::RRRG, true,  { 0 },          47 },
			{ T::cTFASTC_LDR_4x4_RGBA,  L::RGBA, true,  { 0, 1, 2, 3 }, 34 },
			{ T::cTFASTC_LDR_6x6_RGBA,  L::RGBA, true,  { 0, 1, 2, 3 }, 31 },
			{ T::cTFASTC_LDR_6x6_RGBA,  L::RG01, false, { 0, 1, 2, 3 }, 36 }, // ASTC NormalRG
			{ T::cTFASTC_LDR_6x6_RGBA,  L::RRRG, true,  { 0, 3 },       36 }, // ASTC NormalLA
			{ T::cTFASTC_LDR_6x6_RGBA,  L::AGR,  true,  { 0, 1, 2, 3 }, 36 }, // ASTC NormalAG
			{ T::cTFASTC_LDR_8x8_RGBA,  L::RGBA, true,  { 0, 1, 2, 3 }, 29 },
			{ T::cTFASTC_LDR_10x10_RGBA, L::RGBA, true, { 0, 1, 2, 3 }, 27 },
			{ T::cTFASTC_LDR_12x12_RGBA, L::RGBA, true, { 0, 1, 2, 3 }, 25 },
		};

		// Odd dimensions, so partial blocks are exercised for every block size.
		const FImage Image = MakeImage(67, 45);
		for (const FNativeCase& Case : Cases)
		{
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)Case.Target;
			Settings.Layout = Case.Layout;
			Settings.bHasAlpha = Case.bHasAlpha;
			const std::string Label = std::string(basist::basis_get_format_name(Case.Target)) + " layout " + std::to_string((int)Case.Layout);

			FResult Result;
			if (!EncodeImage(Image, Settings, Result, Label))
			{
				continue;
			}
			Check(!Result.bPayload, Label + ": native request returned a payload");
			const uint64_t Expect = BasisEncoder::NativeSliceSize(Settings.TranscoderTarget, Image.Width, Image.Height);
			if (Result.Data.size() != Expect)
			{
				Fail(Label + ": " + std::to_string(Result.Data.size()) + " bytes, expected " + std::to_string(Expect));
				continue;
			}

			std::vector<int> Channels = Case.Channels;
			// Two-channel targets put the intermediate's alpha in G: check it against the source G.
			const bool bTwoChannel = Case.Target == T::cTFBC5_RG || Case.Target == T::cTFETC2_EAC_RG11;
			double P = SlicePsnr(Result.Data.data(), Case.Target, Image, 0, Case.Layout, Case.bHasAlpha, Channels);
			if (bTwoChannel)
			{
				// Decoded G should equal the layout's A (= source G).
				basisu::gpu_image Gpu;
				Gpu.init(basist::basis_get_basisu_texture_format(Case.Target), Image.Width, Image.Height);
				std::memcpy(Gpu.get_ptr(), Result.Data.data(), Gpu.get_size_in_bytes());
				basisu::image Decoded;
				Gpu.unpack(Decoded, false);
				double Sum = 0;
				uint64_t Count = 0;
				for (uint32_t Y = 0; Y < Image.Height; ++Y)
					for (uint32_t X = 0; X < Image.Width; ++X)
					{
						const double D = (double)Decoded(X, Y)[1] - Image.At(0, X, Y, 1);
						Sum += D * D;
						++Count;
					}
				const double PG = Psnr(Sum, Count);
				std::printf("  %-28s G %.1f dB\n", Label.c_str(), PG);
				Check(PG >= Case.MinPsnr, Label + ": G channel " + std::to_string(PG) + " dB");
			}
			const int32_t Intermediate = BasisEncoder::IntermediateFormat(Settings);
			std::printf("  %-28s %.1f dB (%s, %zu bytes)\n", Label.c_str(), P,
				Intermediate < 0 ? "direct" : BasisEncoder::IntermediateFormatName(Intermediate), Result.Data.size());
			Check(P >= Case.MinPsnr, Label + ": " + std::to_string(P) + " dB, want >= " + std::to_string(Case.MinPsnr));
		}
	}

	void TestSlices()
	{
		const FImage Image = MakeImage(40, 24, 3);
		FSettings Settings;
		Settings.TranscoderTarget = (int32_t)transcoder_texture_format::cTFBC7_RGBA;
		FResult Result;
		if (!EncodeImage(Image, Settings, Result, "3-slice BC7"))
		{
			return;
		}
		const uint64_t SliceSize = BasisEncoder::NativeSliceSize(Settings.TranscoderTarget, Image.Width, Image.Height);
		Check(Result.Data.size() == SliceSize * 3, "3-slice BC7: wrong total size");
		for (uint32_t S = 0; S < 3 && Result.Data.size() == SliceSize * 3; ++S)
		{
			// Each slice must match its own source, and clearly not its neighbour's.
			const uint8_t* Blocks = Result.Data.data() + SliceSize * S;
			const double Own = SlicePsnr(Blocks, transcoder_texture_format::cTFBC7_RGBA, Image, S, EChannelLayout::RGBA, true, { 0, 1, 2, 3 });
			const double Other = SlicePsnr(Blocks, transcoder_texture_format::cTFBC7_RGBA, Image, (S + 1) % 3, EChannelLayout::RGBA, true, { 0, 1, 2, 3 });
			Check(Own >= 30 && Own > Other + 6, "3-slice BC7: slice " + std::to_string(S) + " at " + std::to_string(Own)
				+ " dB against its source, " + std::to_string(Other) + " dB against the next slice");
		}
	}

	void TestPayloads()
	{
		using T = transcoder_texture_format;
		const FImage Image = MakeImage(256, 256, 1, true);

		struct FPayloadCase { T Target; std::vector<int> Channels; double MinPsnr; bool bXUBC7 = false; };
		const std::vector<FPayloadCase> Cases =
		{
			{ T::cTFBC7_RGBA,          { 0, 1, 2, 3 }, 34 },
			{ T::cTFBC7_RGBA,          { 0, 1, 2, 3 }, 34, true },
			{ T::cTFBC3_RGBA,          { 0, 1, 2, 3 }, 32 },
			{ T::cTFETC2_RGBA,         { 0, 1, 2, 3 }, 30 },
			{ T::cTFASTC_LDR_6x6_RGBA, { 0, 1, 2, 3 }, 34 },
			{ T::cTFASTC_LDR_4x4_RGBA, { 0, 1, 2, 3 }, 38 },
		};
		for (const FPayloadCase& Case : Cases)
		{
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)Case.Target;
			Settings.Output = EOutput::Payload;
			Settings.bBC7PayloadAsXUBC7 = Case.bXUBC7;
			const std::string Label = std::string("payload ") + basist::basis_get_format_name(Case.Target) + (Case.bXUBC7 ? " (XUBC7)" : "");

			FResult Result;
			if (!EncodeImage(Image, Settings, Result, Label))
			{
				continue;
			}
			const uint64_t NativeSize = BasisEncoder::NativeSliceSize(Settings.TranscoderTarget, Image.Width, Image.Height);
			if (!Result.bPayload)
			{
				Fail(Label + ": fell back to native: " + Result.Note);
				continue;
			}
			Check(Result.Data.size() <= NativeSize, Label + ": payload larger than native");

			// The device sees the payload at the start of a native-sized buffer.
			std::vector<uint8_t> Buffer(NativeSize, 0xCD);
			std::memcpy(Buffer.data(), Result.Data.data(), Result.Data.size());

			BasisTranscodeCore::FPayloadInfo Info;
			if (!BasisTranscodeCore::GetPayloadInfo(Buffer.data(), Buffer.size(), Info))
			{
				Fail(Label + ": GetPayloadInfo rejected the payload");
				continue;
			}
			Check(Info.Width == Image.Width && Info.Height == Image.Height, Label + ": wrong payload dimensions");
			Check(Info.PayloadSize == Result.Data.size(), Label + ": PayloadSize " + std::to_string(Info.PayloadSize) + " != " + std::to_string(Result.Data.size()));
			Check(Info.SourceFormat == BasisEncoder::IntermediateFormat(Settings), Label + ": unexpected intermediate");
			Check(BasisTranscodeCore::CanTranscode(Buffer.data(), Buffer.size(), (int32_t)Case.Target), Label + ": CanTranscode false");

			std::vector<uint8_t> Native(BasisTranscodeCore::TranscodedSize((int32_t)Case.Target, Info.Width, Info.Height));
			Check(Native.size() == NativeSize, Label + ": TranscodedSize disagrees with NativeSliceSize");
			if (!BasisTranscodeCore::Transcode(Buffer.data(), Buffer.size(), (int32_t)Case.Target, Native.data(), Native.size(), false))
			{
				Fail(Label + ": Transcode failed");
				continue;
			}
			const double P = SlicePsnr(Native.data(), Case.Target, Image, 0, EChannelLayout::RGBA, true, Case.Channels);
			std::printf("  %-28s %.1f dB, %zu of %llu native bytes (%s)\n", Label.c_str(), P, Result.Data.size(),
				(unsigned long long)NativeSize, BasisEncoder::IntermediateFormatName(Info.SourceFormat));
			Check(P >= Case.MinPsnr, Label + ": " + std::to_string(P) + " dB");
			if (!GFixtureDir.empty())
			{
				WriteFixture(std::string(basist::basis_get_format_name(Case.Target)) + (Case.bXUBC7 ? "_XUBC7" : ""), (int32_t)Case.Target, Buffer, Native);
			}

			// Too small a destination must be refused, not overrun.
			Check(!BasisTranscodeCore::Transcode(Buffer.data(), Buffer.size(), (int32_t)Case.Target, Native.data(), Native.size() - 1, false),
				Label + ": accepted a short destination");
		}

		// A payload cannot reach ASTC at a different block size.
		{
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)T::cTFBC7_RGBA;
			Settings.Output = EOutput::Payload;
			FResult Result;
			if (EncodeImage(Image, Settings, Result, "payload reach") && Result.bPayload)
			{
				Check(!BasisTranscodeCore::CanTranscode(Result.Data.data(), Result.Data.size(), (int32_t)T::cTFASTC_LDR_4x4_RGBA),
					"6x6 payload claims to reach ASTC 4x4");
			}
		}

		// Lower quality makes smaller payloads.
		{
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)T::cTFBC7_RGBA;
			Settings.Output = EOutput::Payload;
			FResult Lossless, Lossy;
			Settings.Quality = 100;
			EncodeImage(Image, Settings, Lossless, "payload q100");
			Settings.Quality = 60;
			EncodeImage(Image, Settings, Lossy, "payload q60");
			std::printf("  payload BC7 q100 %zu bytes, q60 %zu bytes\n", Lossless.Data.size(), Lossy.Data.size());
			Check(Lossy.bPayload && Lossless.bPayload && Lossy.Data.size() < Lossless.Data.size(), "quality 60 payload is not smaller than quality 100");
		}

		// Tiny mips: the payload's header alone outweighs the blocks, so native blocks come back.
		{
			const FImage Tiny = MakeImage(4, 4);
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)T::cTFBC1_RGB;
			Settings.Output = EOutput::Payload;
			FResult Result;
			if (EncodeImage(Tiny, Settings, Result, "tiny payload"))
			{
				Check(!Result.bPayload && !Result.Note.empty(), "tiny payload: expected a native fallback with a note");
				Check(Result.Data.size() == 8, "tiny payload: fallback is not one BC1 block");
			}
		}

		// Multi-slice images never produce payloads.
		{
			const FImage Array = MakeImage(64, 64, 2);
			FSettings Settings;
			Settings.TranscoderTarget = (int32_t)T::cTFBC7_RGBA;
			Settings.Output = EOutput::Payload;
			FResult Result;
			if (EncodeImage(Array, Settings, Result, "array payload"))
			{
				Check(!Result.bPayload, "array payload: payload emitted for 2 slices");
				Check(Result.Data.size() == 2 * BasisEncoder::NativeSliceSize(Settings.TranscoderTarget, 64, 64), "array payload: wrong native size");
			}
		}
	}

	void TestPayloadSniff()
	{
		// Native blocks and noise must never be mistaken for a payload.
		const FImage Image = MakeImage(64, 64);
		FSettings Settings;
		Settings.TranscoderTarget = (int32_t)transcoder_texture_format::cTFBC7_RGBA;
		FResult Native;
		BasisTranscodeCore::FPayloadInfo Info;
		if (EncodeImage(Image, Settings, Native, "sniff"))
		{
			Check(!BasisTranscodeCore::GetPayloadInfo(Native.Data.data(), Native.Data.size(), Info), "native BC7 blocks sniffed as a payload");
		}
		std::vector<uint8_t> Noise(4096);
		uint32_t Seed = 7;
		for (uint8_t& Byte : Noise) { Seed = Seed * 1103515245u + 12345u; Byte = (uint8_t)(Seed >> 16); }
		Check(!BasisTranscodeCore::GetPayloadInfo(Noise.data(), Noise.size(), Info), "noise sniffed as a payload");
		Check(!BasisTranscodeCore::GetPayloadInfo(Noise.data(), 10, Info), "short buffer sniffed as a payload");

		// A corrupted header must be rejected (the header checksum is checked).
		Settings.Output = EOutput::Payload;
		const FImage Big = MakeImage(128, 128);
		FResult Payload;
		if (EncodeImage(Big, Settings, Payload, "sniff payload") && Payload.bPayload)
		{
			Payload.Data[20] ^= 0x5A;
			Check(!BasisTranscodeCore::GetPayloadInfo(Payload.Data.data(), Payload.Data.size(), Info), "corrupted payload header accepted");
		}
	}

	void TestInvalidInput()
	{
		const FImage Image = MakeImage(8, 8);
		FSettings Settings;
		FResult Result;
		Settings.TranscoderTarget = -1;
		Check(!BasisEncoder::Encode(Image.BGRA.data(), 8, 8, 1, Settings, Result) && !Result.Error.empty(), "invalid target accepted");
		Settings.TranscoderTarget = (int32_t)transcoder_texture_format::cTFRGBA32;
		Check(!BasisEncoder::Encode(Image.BGRA.data(), 8, 8, 1, Settings, Result), "uncompressed target accepted");
		Settings.TranscoderTarget = (int32_t)transcoder_texture_format::cTFBC6H;
		Check(!BasisEncoder::Encode(Image.BGRA.data(), 8, 8, 1, Settings, Result), "HDR target accepted");
		Settings.TranscoderTarget = (int32_t)transcoder_texture_format::cTFBC1_RGB;
		Check(!BasisEncoder::Encode(nullptr, 8, 8, 1, Settings, Result), "null image accepted");
		Check(BasisEncoder::NativeSliceSize((int32_t)transcoder_texture_format::cTFASTC_LDR_6x6_RGBA, 512, 512) == 86u * 86u * 16u,
			"ASTC 6x6 native size uses the wrong block grid");
	}

	void TestConcurrency()
	{
		// Unreal compresses textures in parallel and uploads from several threads.
		const FImage Image = MakeImage(96, 64);
		std::atomic<int> Failures{ 0 };
		std::vector<std::thread> Threads;
		for (int I = 0; I < 8; ++I)
		{
			Threads.emplace_back([&Image, &Failures, I]()
			{
				BasisTranscodeCore::Init();
				FSettings Settings;
				Settings.TranscoderTarget = (int32_t)(I % 2 ? transcoder_texture_format::cTFBC7_RGBA : transcoder_texture_format::cTFASTC_LDR_6x6_RGBA);
				Settings.Output = I % 4 < 2 ? EOutput::Payload : EOutput::Native;
				FResult Result;
				if (!BasisEncoder::Encode(Image.BGRA.data(), Image.Width, Image.Height, 1, Settings, Result))
				{
					++Failures;
					return;
				}
				if (Result.bPayload)
				{
					std::vector<uint8_t> Native(BasisTranscodeCore::TranscodedSize(Settings.TranscoderTarget, Image.Width, Image.Height));
					if (!BasisTranscodeCore::Transcode(Result.Data.data(), Result.Data.size(), Settings.TranscoderTarget, Native.data(), Native.size(), I % 3 == 0))
					{
						++Failures;
					}
				}
			});
		}
		for (std::thread& Thread : Threads)
		{
			Thread.join();
		}
		Check(Failures == 0, "concurrent encode/transcode failed " + std::to_string(Failures.load()) + " times");
	}
}

// Usage: basis_plugin_tests [--write-fixtures <existing dir>]
int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--write-fixtures") == 0)
	{
		GFixtureDir = argv[2];
	}

	std::printf("basis_universal %s\n", BasisEncoder::LibraryVersion());
	const std::vector<std::pair<const char*, std::function<void()>>> Tests =
	{
		{ "native formats", TestNativeFormats },
		{ "slices", TestSlices },
		{ "payloads", TestPayloads },
		{ "payload sniff", TestPayloadSniff },
		{ "invalid input", TestInvalidInput },
		{ "concurrency", TestConcurrency },
	};
	for (const auto& Test : Tests)
	{
		std::printf("[%s]\n", Test.first);
		Test.second();
	}
	std::printf(GFailures ? "%d FAILED\n" : "all passed\n", GFailures);
	return GFailures ? 1 : 0;
}
