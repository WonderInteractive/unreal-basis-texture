# Third-party notices

Everything under `Source/ThirdParty/basis_universal` is vendored unmodified from
[basis_universal](https://github.com/BinomialLLC/basis_universal) at the commit recorded in
`Source/ThirdParty/basis_universal.version` (`Tools/update-basisu.sh --check` verifies it). The
license texts are in `Source/ThirdParty/basis_universal/LICENSES`.

| Component | Copyright | License | Where |
|---|---|---|---|
| Basis Universal | Binomial LLC | Apache-2.0 | `basis_universal/` (`LICENSE`, `NOTICE`) |
| Zstandard | Meta Platforms, Inc. and affiliates | BSD-3-Clause (zstd is dual-licensed BSD/GPLv2; used here under BSD) | `basis_universal/zstd` (`zstd/LICENSE`) |
| Android ASTC decompressor | The Android Open Source Project | Apache-2.0 | `basis_universal/encoder/3rdparty/android_astc_decomp.cpp` (not compiled: `BASISU_DISABLE_ANDROID_ASTC_DECOMP`) |
| tinyexr, exr_reader, streamreader | Syoyo Fujita and contributors | BSD-3-Clause | `basis_universal/encoder/3rdparty` |
| QOI | Dominic Szablewski | MIT | `basis_universal/encoder/3rdparty/qoi.h` |
| tinydds | DeanoC | MIT | `basis_universal/encoder/3rdparty/tinydds.h` |
| miniz (fork) | Binomial LLC, from the public-domain original | Apache-2.0 | `basis_universal/encoder/basisu_miniz.h` |
| jpgd | Richard Geldreich; Binomial LLC | Public domain or Apache-2.0 | `basis_universal/encoder/jpgd.cpp` |
| pvpngreader | Dedicated to the public domain by its authors | Unlicense | `basis_universal/encoder/pvpngreader.cpp` |

## What a shipped game contains

Only the `BasisTranscoder` module is in a cooked game, and it compiles only Basis Universal's
transcoder and zstd's decoder (or no zstd, with `ZstdModule`). A game that ships it should credit
Basis Universal (Apache-2.0, with its NOTICE, which `NOTICE` quotes) and Zstandard (BSD-3-Clause) in its
third-party notices. Everything else is compiled only into the editor and cooker.
