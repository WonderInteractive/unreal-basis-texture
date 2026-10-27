# Changelog

## 1.0.0 (unreleased)

First public release, with basis_universal 2.50.

- `TextureFormatBasis` (`TFB_` formats) encodes native blocks:
  - BC1 to BC5 and EAC R11/RG11 encoded directly.
  - BC7 through XUBC7.
  - ETC2 from UASTC, with EAC alpha encoded directly.
  - ASTC at the block size the engine's ASTC encoders would choose.
- `TextureFormatBasisPayload` (`TFBP_` formats) cooks supercompressed payloads.
- `BasisTranscoder` transcodes them on the device, through the `IBasisTranscoder` modular feature.
- Standalone tests of the encoder and the runtime transcoder, with CI on Linux, macOS and Windows.
