# Basis Universal Texture Encoding for Unreal Engine

An Unreal Engine plugin that encodes textures at cook time with
[Basis Universal](https://github.com/BinomialLLC/basis_universal). It works in one of two ways:

- **Native blocks** (`TextureFormatBasis`). A drop-in replacement for the engine's BC, ETC2 and ASTC
  encoders. The cooked data is ordinary GPU blocks, so the runtime needs nothing extra.
- **Payloads** (`TextureFormatBasisPayload`). Each mip is cooked as a compact, supercompressed
  `.basis` payload that the device transcodes to native blocks at upload, through the
  `BasisTranscoder` runtime module. This shrinks downloads for platforms that stream their content,
  but the platform's RHI must call the transcoder (see [Payloads](#payloads)).

Status: **beta**. The encoder and transcoder cores are covered by standalone tests (`Tests/`) that
CI runs on Linux, macOS and Windows. The Unreal modules are developed against Unreal Engine 5.8 and
ue6-main; other engine versions are untested.

## Installation

1. Put the plugin in your project's `Plugins/` folder (or the engine's), and enable it in the
   `.uproject`:

   ```json
   "Plugins": [ { "Name": "TextureFormatBasis", "Enabled": true } ]
   ```

2. Choose the encoder for a target platform in that platform's Engine config, for example
   `Config/Android/AndroidEngine.ini`:

   ```ini
   [AlternateTextureCompression]
   TextureCompressionFormat="TextureFormatBasis"
   TextureCompressionFormatWithVersion="TextureFormatBasis"
   ```

   Set both keys. The engine sends every texture that records an Oodle SDK version (new textures
   do) to `TextureCompressionFormatWithVersion`, and if that key is missing it uses
   `TextureFormatOodle` for them.

   For payloads, name `TextureFormatBasisPayload` instead, but only for a platform whose RHI
   transcodes them. Never use it for the platform the editor runs on: the editor would upload
   payloads as if they were blocks.

The engine applies the plugin only to the format names it lists (below). Everything else stays with
the engine's own encoders: DXT3, the HDR formats (BC6H, ASTC HDR) and the uncompressed formats.

## Formats

| Engine format | Pixel format | Native encode |
|---|---|---|
| `DXT1`, `DXT5`, `AutoDXT` | BC1, BC3 | Direct BC1/BC3 encode |
| `DXT5n` | BC3 (X in alpha, Y in green) | Direct BC3 encode |
| `BC4`, `BC5` | BC4, BC5 | Direct encode with an endpoint refit |
| `BC7` | BC7 | XUBC7: a full BC7 encode, copied losslessly |
| `ETC2_RGB`, `ETC2_RGBA`, `AutoETC2` | ETC2 | ETC1-compatible colour from UASTC's ETC1 hints; EAC alpha encoded directly |
| `ETC2_R11`, `ETC2_RG11` | EAC R11, RG11 | Direct EAC encode |
| `ASTC_RGB`, `ASTC_RGBA`, `ASTC_RGBAuto`, `ASTC_RGBA_HQ` | ASTC 4x4 to 12x12 | Direct ASTC encode at the block size |
| `ASTC_NormalAG`, `ASTC_NormalRG`, `ASTC_NormalLA` | ASTC 6x6 | Direct ASTC encode |
| `ASTC_NormalRG_Precise` | `PF_ASTC_6x6_NORM_RG` | Direct ASTC encode, luminance-alpha (the RHI must support the `NORM_RG` formats) |

For ASTC the plugin picks the block size the engine's ASTC encoders would pick:

- 6x6 for normal maps, and 4x4 for other virtual textures.
- Otherwise the texture's Compression Quality when it is set (0 to 4 are 12x12, 10x10, 8x8, 6x6,
  4x4).
- Otherwise `[/Script/UnrealEd.CookerSettings] DefaultASTCQualityBySize` (default 3, 6x6), or
  `DefaultASTCQualityBySizeHQ` (default 4, 4x4) for `ASTC_RGBA_HQ`. The `-astcqualitybysize=` and
  `-astcqualitybysizehq=` switches override them.

### Quality

Compared with transcoding from UASTC, the usual Basis Universal route, on Kodak photographs
(kodim03, 05 and 13, PSNR over the channels each format carries):

| Target | Gain over UASTC | Time |
|---|---|---|
| BC1, BC3 | +0.2 to +0.6 dB | 7 to 30 times faster |
| BC4, BC5 | +1.0 to +3.1 dB | 17 to 55 times faster |
| EAC R11, RG11 | +1.3 to +3.8 dB | About the same at Effort 1 |
| BC7 (XUBC7) | +0.5 to +0.8 dB | 4 to 8 times faster |

ETC1 colour comes from UASTC, whose per-block ETC1 hints beat a direct encode with basisu's ETC1
optimizer in our tests. Raising `Effort` to 4 adds about 1 dB there, at 20 times the time.

## Settings

Set these in `[TextureFormatBasisSettings]` in the project's Engine config (`Config/DefaultEngine.ini`).
The editor and cooker read them once at startup, and they apply to every platform that uses the
plugin. They are part of each texture's derived-data key, so changing one re-encodes only the
textures it affects.

| Key | Default | Meaning |
|---|---|---|
| `Effort` | `1` | Encoder effort, 0 to 10, or -1 for the library's default (3). It applies to the ASTC, XUBC7 and UASTC encodes and to payloads, and sets the EAC search radius. Effort 1 costs ASTC about 0.1 dB against 3, at a fifth of the time; for UASTC and XUBC7 it is as good or better. |
| `PayloadQuality` | `100` | Payload quality, 1 to 100. 100 supercompresses the intermediate losslessly; lower values make smaller, lossy payloads. |
| `PayloadBlockSize` | `6x6` | The XUASTC block size of payloads for targets other than ASTC (`WxH`). ASTC payloads use the target's block size. |
| `bBC7PayloadAsXUBC7` | `False` | Encode BC7 payloads as XUBC7 (supercompressed BC7) instead of XUASTC. Larger, but better at high quality and cheaper to transcode. |
| `ZstdModule` | none | See [zstd](#zstd). Read by the build from the target platform's Engine config. |

## Payloads

A payload mip is exactly the size of the native mip it replaces: the `.basis` file at its start and
zeros after it, which compress to almost nothing in a pak. The plugin writes payloads only for plain 2D
textures. Virtual textures, CPU-accessible textures, cubemaps, arrays and volumes get native blocks,
because code other than the RHI reads them. So does any mip whose payload would not fit (the
smallest mips).

Payload intermediates are XUASTC at the target's block size for ASTC targets, and XUASTC at
`PayloadBlockSize` (or XUBC7, for BC7 with `bBC7PayloadAsXUBC7`) for everything else. A target the
intermediate cannot reach gets native blocks.

### RHI integration

`BasisTranscoder` is a runtime module that loads at `PostConfigInit`, before the RHI starts. It
exposes `IBasisTranscoder` (`Source/BasisTranscoder/Public/BasisTranscoder.h`) as a modular feature.

- An engine module such as an RHI cannot depend on a plugin module. It includes the header instead,
  which needs nothing from the module at link time:

  ```csharp
  // In the RHI's Build.cs
  PrivateIncludePaths.Add(Path.Combine(<plugin directory>, "Source", "BasisTranscoder", "Public"));
  ```

- A plugin or game module can add `BasisTranscoder` to its dependencies instead.

Look the transcoder up once on the game thread, during RHI initialisation, and keep the pointer.
Then check each mip of a texture as it is uploaded:

```cpp
#include "BasisTranscoder.h"

// At RHI initialisation. Null when the plugin is not enabled.
IBasisTranscoder* Transcoder = IBasisTranscoder::Get();

// For each mip upload, from any thread.
IBasisTranscoder::FPayloadInfo Info;
if (Transcoder && Transcoder->GetPayloadInfo(MipData, Info))
{
	TArray64<uint8> Native;
	Native.SetNumUninitialized(MipData.GetSize()); // a payload mip is native-sized
	if (Transcoder->Transcode(MipData, PixelFormat, MakeMemoryView(Native)))
	{
		// upload Native in place of MipData
	}
}
```

`GetPayloadInfo` checks only the header and its checksum, so it is cheap and safe to call on native
blocks, which fail it. `Transcode` is thread-safe. Its `bHighQuality` argument selects the slower,
better XUASTC-to-BC7 path and changes nothing else.

If a platform cooks payloads, the game must enable the plugin, and the RHI should treat a null
transcoder as an error rather than uploading payloads as blocks.

## zstd

Basis Universal supercompresses payloads with zstd. The plugin compiles the vendored zstd: its
decoder in a game, the whole library in the editor. If the target links another zstd, the two
copies' symbols can collide (in a monolithic build, for one). To use the other copy instead, which
must be zstd 1.4 or later, name the module that provides it:

```ini
[TextureFormatBasisSettings]
ZstdModule=MyZstd
```

That module's zstd must be linkable from the plugin's binaries: an External module wrapping a static
library, or any module in a monolithic target.

## Limitations

- **LDR only.** Sources are converted to 8-bit BGRA. The HDR formats and DXT3 stay with the
  engine's encoders.
- **Payloads need a transcoding RHI.** No stock RHI transcodes them.
- **One set of settings.** The settings are global, not per platform.
- **Output varies by architecture.** basis_universal's SSE paths are not bit-exact with its scalar
  ones, so x64 and arm64 cookers can produce different bytes for the same texture. Both are valid,
  but a derived-data cache shared between them keeps whichever version was written first.

## Development

The encoder and transcoder cores (`BasisEncoder.*`, `BasisTranscodeCore.*`) have no Unreal types,
so they build and are tested on their own. The tests use the same wrappers and configuration as the
modules:

```bash
cmake -S Tests -B Build/Tests -DCMAKE_BUILD_TYPE=Release
cmake --build Build/Tests --config Release
ctest --test-dir Build/Tests -C Release --output-on-failure
```

`basis_plugin_tests` encodes every format and layout, checks quality against a floor, and writes
payload fixtures. `basis_runtime_tests` transcodes those fixtures through the runtime build of the
transcoder (no encoder, no KTX2) and checks it matches the encoder's own result byte for byte.

basis_universal is vendored unmodified in `Source/ThirdParty/basis_universal` (see
[Source/ThirdParty/README.md](Source/ThirdParty/README.md)). To upgrade it, run
`Tools/update-basisu.sh <tag>`, then the tests.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). The vendored components and their
licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Basis Universal is a trademark of Binomial LLC. This project is not affiliated with or endorsed by
Binomial LLC.
