// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

// The encoder: an alternate texture format that encodes with Basis Universal (see the README).
public class TextureFormatBasis : ModuleRules
{
	public TextureFormatBasis(ReadOnlyTargetRules Target) : base(Target)
	{
		// basis_universal is compiled one source per translation unit through the generated wrappers
		// in Private/BasisUniversal: upstream relies on file-scoped using-directives, and its sources
		// must not see engine headers.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		IWYUSupport = IWYUSupport.None;

		string ThirdPartyDir = Path.Combine(PluginDirectory, "Source", "ThirdParty");
		PrivateIncludePaths.Add(ThirdPartyDir);
		PrivateIncludePaths.Add(Path.Combine(ThirdPartyDir, "basis_universal"));

		PublicDependencyModuleNames.Add("Core");

		PrivateIncludePathModuleNames.AddRange(new string[]
		{
			"TargetPlatform",
			"TextureCompressor",
			"TextureFormat",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// The pixel format mapping, and the definitions that say whether this module compiles
			// its own copy of the transcoder and zstd (BasisTranscoder.Build.cs).
			"BasisTranscoder",
			"DerivedDataCache",
			"ImageCore",
			"TextureBuild",
		});
	}
}
