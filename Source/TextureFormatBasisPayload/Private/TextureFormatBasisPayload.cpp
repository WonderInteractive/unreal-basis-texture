// Copyright Wonder Interactive, Inc. Licensed under the Apache License, Version 2.0.

#include "DerivedDataBuildFunctionFactory.h"
#include "DerivedDataBuildVersion.h"
#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatModule.h"
#include "Modules/ModuleManager.h"
#include "TextureBuildFunction.h"
#include "TextureFormatBasis.h"

namespace UE::TextureFormatBasis::Private
{
	/** The cache build function, named by the engine's convention from the module's name. */
	class FBasisPayloadTextureBuildFunction final : public FTextureBuildFunction
	{
		virtual const UE::FUtf8SharedString& GetName() const override
		{
			static const UE::FUtf8SharedString Name(UTF8TEXTVIEW("BasisPayloadTexture"));
			return Name;
		}

		virtual void GetVersion(UE::DerivedData::FBuildVersionBuilder& Builder, ITextureFormat*& OutTextureFormatVersioning) const override
		{
			static const FGuid Version(TEXT("61f72c82-30cb-47a6-83ba-c1d0d3017ac2"));
			Builder << Version;
			UE::TextureFormatBasis::AddBuildVersion(Builder);
			OutTextureFormatVersioning = FModuleManager::GetModuleChecked<ITextureFormatModule>(TEXT("TextureFormatBasisPayload")).GetTextureFormat();
		}
	};
}

/** Encodes to payloads the device transcodes: a platform selects it with the TFBP_ prefix. */
class FTextureFormatBasisPayloadModule final : public ITextureFormatModule
{
public:
	virtual void StartupModule() override
	{
		TextureFormat = UE::TextureFormatBasis::CreateTextureFormat(true);
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

	static inline UE::DerivedData::TBuildFunctionFactory<UE::TextureFormatBasis::Private::FBasisPayloadTextureBuildFunction> BuildFunctionFactory;
};

IMPLEMENT_MODULE(FTextureFormatBasisPayloadModule, TextureFormatBasisPayload)
