// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreTypes.h"
#include "Templates/UniquePtr.h"

class ITextureFormat;
namespace UE::DerivedData { class FBuildVersionBuilder; }

namespace UE::TextureFormatBasis
{
	/**
	 * The Basis Universal texture format behind both encoder modules: native blocks (the
	 * TextureFormatBasis module, TFB_ format names) or payloads the device transcodes
	 * (TextureFormatBasisPayload, TFBP_ names).
	 */
	TEXTUREFORMATBASIS_API TUniquePtr<ITextureFormat> CreateTextureFormat(bool bPayload);

	/** Adds the vendored library's version to a texture build function's version, so that a
	 *  library upgrade invalidates cached textures (the format's version covers the plugin's own
	 *  changes). */
	TEXTUREFORMATBASIS_API void AddBuildVersion(UE::DerivedData::FBuildVersionBuilder& Builder);
}
