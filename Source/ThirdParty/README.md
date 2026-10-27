# Vendored basis_universal

`basis_universal/` is [basis_universal](https://github.com/BinomialLLC/basis_universal), unmodified,
at the tag and commit in `basis_universal.version`. Only `LICENSE`, `NOTICE`, `LICENSES/`,
`encoder/`, `transcoder/` and `zstd/` are kept. Never edit it by hand:

```bash
Tools/update-basisu.sh <tag-or-commit>   # re-vendor and regenerate the wrappers
Tools/update-basisu.sh --check           # verify the tree matches the recorded commit
```

Everything Unreal needs is done beside the tree:

- **Wrappers.** `Source/*/Private/BasisUniversal/` holds one generated translation unit per library
  source. Upstream relies on file-scoped using-directives, so the sources are never merged into
  unity files, and they never see an engine header.
- **`BasisUniversalBegin.h` and `BasisUniversalEnd.h`.** Every wrapper and every plugin file that
  includes a library header puts them around it.
  - Begin sets the library configuration: no PVRTC, ATC or FXT1 targets, no OpenCL, SSE4.1 kernels
    on x64 only, and no KTX2 in a transcoder-only build.
  - Begin also silences the library's warnings (the engine builds with warnings as errors), and End
    restores them.
- **Build.cs.** `BasisTranscoder.Build.cs` decides which binary carries the transcoder and zstd
  (`BASIS_UE_*` definitions). See the comments there.
