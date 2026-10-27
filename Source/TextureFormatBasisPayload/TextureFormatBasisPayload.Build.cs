// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// The payload variant of TextureFormatBasis, as its own module because a platform names one
// module in [AlternateTextureCompression]. The encoding itself lives in TextureFormatBasis.
public class TextureFormatBasisPayload : ModuleRules
{
	public TextureFormatBasisPayload(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePathModuleNames.AddRange(new string[]
		{
			"TextureCompressor",
			"TextureFormat",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"DerivedDataCache",
			"TextureBuild",
			"TextureFormatBasis",
		});
	}
}
