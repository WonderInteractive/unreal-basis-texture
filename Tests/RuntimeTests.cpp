// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

// The runtime transcoder as BasisTranscoder compiles it for a game: without the KTX2 code and with
// zstd's decoder only (BASIS_UE_TRANSCODER_ONLY, BASIS_UE_ZSTD_SOURCE=1). It transcodes the
// payloads basis_plugin_tests wrote and must reproduce, byte for byte, what the full library made
// of them.

#include "BasisTranscodeCore.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::printf("usage: basis_runtime_tests <fixture dir>\n");
		return 2;
	}

	int Failures = 0;
	int Count = 0;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(argv[1]))
	{
		if (Entry.path().extension() != ".fixture")
		{
			continue;
		}
		const std::string Name = Entry.path().stem().string();
		std::ifstream File(Entry.path(), std::ios::binary);
		const std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
		uint32_t Header[3] = {};
		if (Bytes.size() < sizeof(Header))
		{
			std::printf("FAIL: %s: truncated\n", Name.c_str());
			++Failures;
			continue;
		}
		std::memcpy(Header, Bytes.data(), sizeof(Header));
		const int32_t Target = (int32_t)Header[0];
		const uint8_t* Buffer = Bytes.data() + sizeof(Header);
		const uint8_t* Expected = Buffer + Header[1];
		if (Bytes.size() != sizeof(Header) + (size_t)Header[1] + Header[2])
		{
			std::printf("FAIL: %s: wrong size\n", Name.c_str());
			++Failures;
			continue;
		}

		BasisTranscodeCore::FPayloadInfo Info;
		std::vector<uint8_t> Native(Header[2]);
		const bool bOk = BasisTranscodeCore::GetPayloadInfo(Buffer, Header[1], Info)
			&& BasisTranscodeCore::CanTranscode(Buffer, Header[1], Target)
			&& BasisTranscodeCore::TranscodedSize(Target, Info.Width, Info.Height) == Native.size()
			&& BasisTranscodeCore::Transcode(Buffer, Header[1], Target, Native.data(), Native.size(), false);
		if (!bOk || std::memcmp(Native.data(), Expected, Native.size()) != 0)
		{
			std::printf("FAIL: %s: %s\n", Name.c_str(), bOk ? "output differs from the full library's" : "transcode failed");
			++Failures;
			continue;
		}
		std::printf("  %-24s %u bytes of payload, identical\n", Name.c_str(), Info.PayloadSize);
		++Count;
	}

	if (Count == 0 && Failures == 0)
	{
		std::printf("FAIL: no fixtures in %s\n", argv[1]);
		++Failures;
	}
	std::printf(Failures ? "%d FAILED\n" : "all passed\n", Failures);
	return Failures ? 1 : 0;
}
