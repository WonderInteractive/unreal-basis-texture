// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

// Transcodes TextureFormatBasisPayload's payloads on the device (see Public/BasisTranscoder.h). It
// also decides, for the whole plugin, which binary carries the library's transcoder and zstd.
public class BasisTranscoder : ModuleRules
{
	public BasisTranscoder(ReadOnlyTargetRules Target) : base(Target)
	{
		// basis_universal is compiled one source per translation unit through the generated wrappers
		// in Private/BasisUniversal: upstream relies on file-scoped using-directives, and its sources
		// must not see engine headers.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		IWYUSupport = IWYUSupport.None;

		PublicDependencyModuleNames.Add("Core");

		string ThirdPartyDir = Path.Combine(PluginDirectory, "Source", "ThirdParty");
		PrivateIncludePaths.Add(ThirdPartyDir);
		PrivateIncludePaths.Add(Path.Combine(ThirdPartyDir, "basis_universal"));

		// Upstream exports nothing, so in a modular build each binary that uses the library carries
		// its own copy and the encoder module compiles its own transcoder and zstd. In a monolithic
		// build they share this module's. The encoder module is UncookedOnly, so it is in the target
		// exactly when the target does not require cooked data.
		bool bMonolithic = Target.LinkType == TargetLinkType.Monolithic;
		bool bEncoderShares = bMonolithic && !Target.bBuildRequiresCookedData;
		string ZstdModule = GetZstdModule(Target);

		PublicDefinitions.Add("BASIS_UE_ENCODER_TRANSCODER=" + (bMonolithic ? "0" : "1"));
		PublicDefinitions.Add("BASIS_UE_ENCODER_ZSTD=" + (bMonolithic || ZstdModule != null ? "0" : "1"));
		// 1: zstd's decoder only. 2: all of zstd, for an encoder that shares it. 0: ZstdModule's.
		PrivateDefinitions.Add("BASIS_UE_ZSTD_SOURCE=" + (ZstdModule != null ? "0" : bEncoderShares ? "2" : "1"));
		// A transcoder no encoder shares leaves out the KTX2 code (see BasisUniversalBegin.h).
		PrivateDefinitions.Add("BASIS_UE_TRANSCODER_ONLY=" + (bEncoderShares ? "0" : "1"));
		if (ZstdModule != null)
		{
			PublicDependencyModuleNames.Add(ZstdModule);
		}
	}

	// [TextureFormatBasisSettings] ZstdModule in the target platform's Engine config: a module that
	// already links zstd 1.4 or later into the target, used instead of the vendored copy. Set it when
	// the target links another zstd, whose symbols would collide with the vendored one. Its zstd must
	// be linkable from this module's binary: an External module wrapping a static library, or any
	// module in a monolithic target.
	static string GetZstdModule(ReadOnlyTargetRules Target)
	{
		ConfigHierarchy Config = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, Target.ProjectFile?.Directory, Target.Platform);
		if (Config.GetString("TextureFormatBasisSettings", "ZstdModule", out string Name) && !string.IsNullOrWhiteSpace(Name))
		{
			return Name.Trim();
		}
		return null;
	}
}
