# Third-party notices

This repository vendors the following upstream projects so the native library
can be rebuilt without network access. The exact revisions used for the
published `v1.0.0` binary are pinned below.

## LSPlant

- Project: LSPlant (ART inline hooking framework)
- Upstream: https://github.com/LSPosed/LSPlant
- Pinned commit: `d8b5d1dbb664abc606644036822e4bb64547edf6`
- Vendored at: `third_party/LSPlant/lsplant/src/main/jni/`
- License: GNU Lesser General Public License v3.0 — see `third_party/LSPlant/LICENSE`
- Local changes: `patches/LSPlant-coloros17-gsi-fixes.patch` (already applied to
  the vendored tree). It adapts ART internals for ColorOS 17 / Android 17.

LSPlant statically links **DexBuilder**:

- Project: DexBuilder (part of LSPlant)
- Pinned commit: `ac7fb2230954ee311808bad469b0db501f31bfb8`
- Vendored at: `third_party/LSPlant/lsplant/src/main/jni/external/dex_builder/`
- License: GNU Lesser General Public License v3.0 — see
  `third_party/LSPlant/lsplant/src/main/jni/external/dex_builder/LICENSE`

DexBuilder in turn uses **parallel-hashmap**:

- Project: parallel-hashmap
- Upstream: https://github.com/greg7mdp/parallel-hashmap
- Pinned commit: `0cd57d29a959256ed66b2afdd1009928fc625d09`
- Vendored at:
  `third_party/LSPlant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap/`
- License: Apache License 2.0 — see the `LICENSE` file in that directory

## Dobby

- Project: Dobby (LSPosed fork; instruction relocation and trampolines)
- Upstream: https://github.com/LSPosed/Dobby
- Pinned commit: `c1da0315d7a2069bde2ab432e45d4ab6df91237a`
- Vendored at: `third_party/DobbyLS/`
- License: Apache License 2.0 — see `third_party/DobbyLS/LICENSE`
- Local changes: `patches/DobbyLS-arm64-absolute64-jumps.patch` (already applied
  to the vendored tree). It replaces range-limited ARM64 trampoline jumps with
  fixed-width absolute jumps.

## Zygisk module API header

- Project: Zygisk public module API (`zygisk.hpp`)
- Author: John "topjohnwu" Wu
- Vendored at: `src/native/third_party/zygisk/zygisk.hpp`
- License: permissive ISC-style terms, reproduced verbatim in the header.

`zygisk.hpp` is only the public module API header (Zygisk API version 5). No
ZygiskNext binaries, loaders, or other loader files are included in this
repository.

## LGPL relinking note

LSPlant and DexBuilder are statically linked into the published
`arm64-v8a.so`. Their complete corresponding source is vendored here, together
with the build scripts, so the library can be modified and relinked. See the
GNU Lesser General Public License v3.0 texts in the locations above.
