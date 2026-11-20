# coloros17-md3-systemui-hook

A small ARM64 Zygisk native library that fixes two visual issues in the
**ColorOS 17** SystemUI when it is themed by the **Material You for ColorOS
v0.2.5** overlay module:

1. **Opaque Quick Settings slider tracks.** The brightness and volume sliders
   (`OplusQsVerticalSeekBar`) normally draw a translucent blurred track. The
   hook replaces that track with an opaque Material You surface that matches the
   QS pills: `system_neutral1_900` in Dark Mode and `system_neutral1_50` in Light
   Mode. The active fill and rounded corners are preserved.
2. **Desaturated Dark Mode shade tint.** The expanded notification shade and QS
   panel receive a clean `system_accent2_900` tint at **70% opacity**
   (`179/255`), instead of the higher-chroma stock mask. Light Mode, the
   keyguard/bouncer, and the stock blur recipes are left alone.
3. **Safe ColorOS OEM trampoline handling.** ColorOS routes some ART entry
   points through `libnyako_core.so` trampolines. The library resolves that OEM
   chain and defers post-application initialization until the post-fork runtime
   is ready, instead of patching too early. LSPlant and Dobby do the actual
   inline hooking and relocation.

The result is a dark shade whose cards, QS pills, and slider tracks share one
consistent surface color, with a calmer, theme-tinted backdrop.

## What is in this repository

This repository contains **only the native helper and its source**:

- `src/native/` — Zygisk entry point, JNI loader, embedded-dex handling, ART ELF
  symbol resolution, and the ColorOS OEM-chain trampoline resolver.
- `src/java/md3/Hooks.java` — the Java runtime hooks compiled to `classes.dex`
  and embedded inside the `.so`.
- `third_party/` — pinned LSPlant, DexBuilder, parallel-hashmap, and Dobby
  sources.
- `patches/` — the ColorOS 17 LSPlant fixes and the ARM64 absolute-jump Dobby
  patch (both already applied to the vendored trees).
- `CMakeLists.txt`, `build.sh` — reproducible ARM64 build.

It does **not** contain the Material You overlay module, any APK, any module
ZIP, `module.prop`, `customize.sh`, `service.sh`, `theme-sync.sh`, `META-INF`,
or any ZygiskNext loader files or binaries.

## Compatibility

| Item | Requirement |
| --- | --- |
| ROM | **ColorOS 17 only** (Android 17 / API 37) |
| CPU | **ARM64 (`arm64-v8a`) only** |
| Process | `com.android.systemui` only (exact process name) |
| Base theme module | **Material You for ColorOS v0.2.5** for ColorOS 17, installed and enabled |
| Zygisk provider | **ZygiskNext v1.5.3 or newer**, or any loader implementing Zygisk module API v5 |

> **Important:** this is a companion add-on, not a standalone theme. The
> **Material You for ColorOS v0.2.5** KernelSU/Magisk overlay module is **not
> hosted in this repository** and is not redistributed here. Obtain the base
> module from its upstream author and install it first.

It is not intended for ColorOS 16 or earlier, AOSP, other OEM ROMs, or 32-bit
devices. It was verified against the ColorOS 17 SystemUI paired with Material
You for ColorOS v0.2.5.

## Install the prebuilt library

1. Download `arm64-v8a.so` (and `arm64-v8a.so.sha256`) from the
   [Releases](../../releases) page.
2. Verify the download:

   ```bash
   sha256sum arm64-v8a.so
   cat arm64-v8a.so.sha256
   ```

   The two values must match.
3. Make sure the **Material You for ColorOS v0.2.5** module is already installed
   and that **ZygiskNext v1.5.3+** (or a compatible Zygisk API v5 loader) is
   active with Zygisk enabled.
4. Replace the base module's helper with the downloaded file:

   ```bash
   adb push arm64-v8a.so /data/local/tmp/arm64-v8a.so
   adb shell su -c '
     mkdir -p /data/adb/modules/material_you_for_coloros/zygisk
     cp /data/local/tmp/arm64-v8a.so \
        /data/adb/modules/material_you_for_coloros/zygisk/arm64-v8a.so
     chown 0:0 /data/adb/modules/material_you_for_coloros/zygisk/arm64-v8a.so
     chmod 0644 /data/adb/modules/material_you_for_coloros/zygisk/arm64-v8a.so
     restorecon -R /data/adb/modules/material_you_for_coloros/zygisk 2>/dev/null || true
   '
   ```

   If your root manager exposes a module folder with a different name, use that
   module's `zygisk/` directory instead. The file must be named exactly
   `arm64-v8a.so` and be readable by the SystemUI process (`0644`).
5. Reboot.
6. Optional check after boot:

   ```bash
   adb logcat -s MD3Runtime
   ```

   A healthy load logs lines such as `ready: final dark scrim tint ...`,
   `solid slider dark ...`, and `dark final scrim: alpha=179/255 ...`.
   Failures are intentionally fail-open (`fail-open: ...`) and never crash
   SystemUI.

To uninstall, delete
`/data/adb/modules/material_you_for_coloros/zygisk/arm64-v8a.so` (or restore the
base module's original helper) and reboot.

## Build from source

### Prerequisites

- Android **NDK r27 or newer** (the reference `v1.0.0` binary was built with
  NDK r29 / `29.0.14206865`)
- Android SDK **build-tools** containing `d8`
- CMake **3.28+** and Ninja
- JDK **17 or 21** (`javac` and `jar`)
- Python 3

Set `ANDROID_NDK_HOME` and `ANDROID_HOME` (or put `d8` on `PATH`), then:

```bash
git clone https://github.com/ampereforest/coloros17-md3-systemui-hook.git
cd coloros17-md3-systemui-hook
./build.sh
```

`build.sh` performs four steps:

1. compiles `src/java/md3/Hooks.java` with `javac --release 8` and converts it
   to `build/dex/classes.dex` with `d8 --min-api 26`;
2. embeds that dex into the native library;
3. builds `arm64-v8a.so` with NDK CMake/Ninja and the 16 KiB page flags
   `-Wl,-z,max-page-size=16384` and `-Wl,-z,common-page-size=16384`;
4. strips DWARF debug sections, verifies every `PT_LOAD` segment is aligned to
   at least 16 KiB, and prints the SHA-256.

Output:

```text
build/native/out/zygisk/arm64-v8a.so
```

Useful options/environment variables:

```bash
./build.sh --keep-debug     # keep DWARF sections (larger file, different hash)
ANDROID_NDK_HOME=/path/to/ndk ./build.sh
D8=/path/to/d8 ./build.sh
MD3_JOBS=8 ./build.sh
MD3_BUILD_DIR=/tmp/md3-build ./build.sh
```

The Java step is deterministic: the same `Hooks.java`, `javac --release 8`, and
`d8 --min-api 26` reproduce the same `classes.dex` byte-for-byte.

The published `v1.0.0` asset is the exact verified reference build and retains
its debug sections:

```text
SHA-256  00bc262b1aa06923c7b36c99d57b05efe0606774fe44f5d469ce360378e4f5e6
Size     11,859,920 bytes
```

Because `build.sh` strips debug sections by default, a normal local build is
functionally equivalent but has a different file hash. Rebuilds can also differ
in embedded source paths, the NDK build-id, and the exact NDK patch release.
Use `--keep-debug` if you want a file shaped like the published asset.

## How it works (short version)

- `module.cpp` is the Zygisk entry point. It selects only the exact
  `com.android.systemui` process, loads the embedded `classes.dex` into the
  app's class loader, registers `md3.Hooks`, and schedules late initialization
  after the OEM post-fork runtime has settled.
- `native-entry-chain.hpp` resolves the ColorOS `libnyako_core.so` entry
  trampoline chain so the ART functions that LSPlant needs can be reached
  safely.
- `art_elf.cpp` parses the on-device `libart.so` ELF to locate the required
  symbols without hard-coded offsets.
- LSPlant installs the Java method hooks (`ScrimView.onDraw` and
  `OplusQsVerticalSeekBar.onDraw`); Dobby provides ARM64 relocation and
  trampoline support.
- `Hooks.java` reads the live Material You tokens from the SystemUI resources,
  so the colors follow the user's current wallpaper palette. Every hook is
  fail-open: an unexpected OEM change disables only that effect, not SystemUI.

## Credits and licenses

LSPlant, DexBuilder, parallel-hashmap, Dobby (LSPosed fork), and the Zygisk
public API header are vendored with their licenses. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for pinned revisions, license
texts, and the LGPL relinking note.

This project is not affiliated with OPPO/OnePlus/ColorOS, LSPosed, or the
Material You for ColorOS module author. Use at your own risk; no warranty is
provided.
