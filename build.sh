#!/usr/bin/env bash
# Reproducible build for the ColorOS 17 SystemUI Zygisk helper.
# Produces: build/native/out/zygisk/arm64-v8a.so
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${MD3_BUILD_DIR:-$ROOT/build}"
KEEP_DEBUG=0

for arg in "$@"; do
    case "$arg" in
        --keep-debug|--no-strip) KEEP_DEBUG=1 ;;
        -h|--help)
            echo "Usage: $0 [--keep-debug]"
            echo "  --keep-debug  keep DWARF debug sections (larger file, different SHA-256)"
            exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

find_ndk() {
    if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then echo "$ANDROID_NDK_HOME"; return; fi
    if [[ -n "${ANDROID_NDK_ROOT:-}" ]]; then echo "$ANDROID_NDK_ROOT"; return; fi
    local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [[ -n "$sdk" && -d "$sdk/ndk" ]]; then
        find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1
        return
    fi
    for candidate in /opt/android-ndk-r* "$HOME"/Android/Sdk/ndk/*; do
        [[ -d "$candidate" ]] && { echo "$candidate"; return; }
    done
    return 1
}

find_d8() {
    if [[ -n "${D8:-}" ]]; then echo "$D8"; return; fi
    if command -v d8 >/dev/null 2>&1; then command -v d8; return; fi
    local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [[ -n "$sdk" && -d "$sdk/build-tools" ]]; then
        find "$sdk/build-tools" -mindepth 2 -maxdepth 2 -name d8 -type f | sort -V | tail -1
        return
    fi
    return 1
}

NDK="$(find_ndk)" || { echo "ERROR: set ANDROID_NDK_HOME (NDK r27+ required)" >&2; exit 1; }
D8_BIN="$(find_d8)" || { echo "ERROR: set D8 or ANDROID_HOME with build-tools (d8 required)" >&2; exit 1; }
CMAKE="${CMAKE_BIN:-$(command -v cmake || true)}"
NINJA="${NINJA_BIN:-$(command -v ninja || true)}"
JAVAC="${JAVAC:-$(command -v javac || true)}"
JAR="${JAR:-$(command -v jar || true)}"

HOST_BIN="$(find "$NDK/toolchains/llvm/prebuilt" -mindepth 2 -maxdepth 2 -type d -name bin 2>/dev/null | head -1)"
[[ -n "$HOST_BIN" && -x "$HOST_BIN/clang++" ]] || {
    echo "ERROR: no clang++ under $NDK/toolchains/llvm/prebuilt" >&2; exit 1; }
[[ -n "$CMAKE" && -x "$CMAKE" ]] || { echo "ERROR: cmake not found (3.28+)" >&2; exit 1; }
[[ -n "$NINJA" && -x "$NINJA" ]] || { echo "ERROR: ninja not found" >&2; exit 1; }
[[ -n "$JAVAC" && -x "$JAVAC" ]] || { echo "ERROR: javac not found (JDK 17 or 21)" >&2; exit 1; }
[[ -n "$JAR" && -x "$JAR" ]] || { echo "ERROR: jar not found (JDK 17 or 21)" >&2; exit 1; }

echo "== Toolchain =="
echo "NDK:   $NDK"
echo "d8:    $D8_BIN"
echo "cmake: $CMAKE"
echo "ninja: $NINJA"
echo "javac: $JAVAC"
"$CMAKE" --version | head -1
"$NINJA" --version

echo "== 1/4 Compile md3.Hooks -> classes.dex =="
rm -rf "$BUILD/classes" "$BUILD/dex"
mkdir -p "$BUILD/classes" "$BUILD/dex"
"$JAVAC" --release 8 -d "$BUILD/classes" "$ROOT/src/java/md3/Hooks.java"
"$JAR" cf "$BUILD/helper.jar" -C "$BUILD/classes" .
"$D8_BIN" --min-api 26 --output "$BUILD/dex" "$BUILD/helper.jar"
DEX="$BUILD/dex/classes.dex"
test -s "$DEX"
echo "classes.dex: $(wc -c < "$DEX") bytes"

echo "== 2/4 Configure + compile arm64-v8a.so =="
"$CMAKE" -S "$ROOT" -B "$BUILD/native" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_static \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DMD3_DEX="$DEX"
"$CMAKE" --build "$BUILD/native" --target md3hook --parallel "${MD3_JOBS:-4}"

SO="$BUILD/native/out/zygisk/arm64-v8a.so"
test -f "$SO"

echo "== 3/4 Strip debug symbols =="
if [[ "$KEEP_DEBUG" -eq 1 ]]; then
    echo "Keeping debug sections (--keep-debug)."
else
    "$HOST_BIN/llvm-strip" --strip-debug "$SO"
    echo "Stripped DWARF debug sections."
fi

echo "== 4/4 Verify ELF and checksum =="
"$HOST_BIN/llvm-readelf" -lW "$SO" | sed -n '1,80p'
"$HOST_BIN/llvm-readelf" -dW "$SO" | sed -n '1,80p'
python3 "$ROOT/src/native/verify_elf.py" "$SO"

echo
echo "Output: $SO"
echo "Size:   $(wc -c < "$SO") bytes"
sha256sum "$SO"
