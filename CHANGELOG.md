# Changelog

## 1.0.0 (unreleased)

First public release, with basis_universal 2.50.

- `TextureFormatBasis` (`TFB_` formats) encodes native blocks:
  - BC1 to BC5 and EAC R11/RG11 encoded directly.
  - BC7 through XUBC7.
  - ETC2 from UASTC, with EAC alpha encoded directly.
  - ASTC at the block size the engine's ASTC encoders would choose.
  - The HDR formats: BC6H and ASTC HDR 4x4 from UASTC HDR 4x4, ASTC HDR 6x6 encoded directly.
- `TextureFormatBasisPayload` (`TFBP_` formats) cooks supercompressed payloads, HDR ones included.
  `PayloadCoding` picks their entropy coding.
- `BasisTranscoder` transcodes them on the device, through the `IBasisTranscoder` modular feature.
- The encoders decode their formats for the editor's platform preview and the derived-data decode function.
- Linear textures are encoded with equal channel weights, sRGB ones with perceptual weights.
- Standalone tests of the encoder and the runtime transcoder, with CI on Linux, macOS and Windows,
  and editor automation tests of the Unreal modules.
