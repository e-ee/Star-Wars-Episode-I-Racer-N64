#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

KNOWN_SHA1="3542d5597c8a56ea8f5c63bceae97a24c4c08d58"
ROM_ARG="${1:-}"

if [ -z "${ANDROID_HOME:-}" ]; then
    if [ -d "$HOME/Library/Android/sdk" ]; then
        export ANDROID_HOME="$HOME/Library/Android/sdk"
    elif [ -d "$HOME/Android/Sdk" ]; then
        export ANDROID_HOME="$HOME/Android/Sdk"
    fi
fi

if [ -z "${ANDROID_HOME:-}" ] || [ ! -d "$ANDROID_HOME" ]; then
    echo "ERROR: Android SDK not found. Set ANDROID_HOME." >&2
    exit 1
fi
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export PATH="$ANDROID_HOME/cmake/3.22.1/bin:$ANDROID_HOME/platform-tools:$PATH"

if [ ! -f "android/local.properties" ]; then
    printf "sdk.dir=%s\n" "$ANDROID_HOME" > android/local.properties
fi

echo "== 1/6 Checking submodules and tools =="
if [ ! -f "lib/N64ModernRuntime/N64Recomp/CMakeLists.txt" ] || [ ! -f "lib/rt64/CMakeLists.txt" ] || [ ! -f "lib/SDL2/CMakeLists.txt" ]; then
    echo "ERROR: Submodules missing in lib/. Run: git submodule update --init --recursive" >&2
    exit 1
fi

for tool in python3 clang clang++ cmake ninja java; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Missing required tool: $tool" >&2
        exit 1
    fi
done

# Ensure host DXC binaries are executable on Unix hosts
chmod +x lib/rt64/src/contrib/dxc/bin/*/dxc-* 2>/dev/null || true

echo "== 2/6 Applying compatibility patches =="
./scripts/apply_patches.sh

echo "== 3/6 Checking ROM =="
if [ -n "$ROM_ARG" ]; then
    if [ ! -f "$ROM_ARG" ]; then
        echo "ERROR: ROM file not found: $ROM_ARG" >&2
        exit 1
    fi
    python3 -I tools/rom_to_z64.py "$ROM_ARG" baserom.z64
fi

if [ ! -f "baserom.z64" ]; then
    echo "ERROR: baserom.z64 not found. Pass ROM path as first argument: ./build.sh /path/to/rom.z64" >&2
    exit 1
fi

ACTUAL_SHA1="$(shasum -a 1 baserom.z64 | awk '{print $1}')"
if [ "$ACTUAL_SHA1" != "$KNOWN_SHA1" ]; then
    echo "ERROR: baserom.z64 SHA-1 mismatch ($ACTUAL_SHA1 != $KNOWN_SHA1)" >&2
    exit 1
fi
echo "   ok  baserom.z64 verified ($KNOWN_SHA1)"

echo "== 4/6 Building N64Recomp & RSPRecomp =="
mkdir -p build
N64RECOMP="build/recomp-tools/N64Recomp"
RSPRECOMP="build/recomp-tools/RSPRecomp"
if [ ! -x "$N64RECOMP" ] || [ ! -x "$RSPRECOMP" ]; then
    cmake -S lib/N64ModernRuntime/N64Recomp -B build/recomp-tools -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_FLAGS="-include cstdlib"
    cmake --build build/recomp-tools --target N64RecompCLI RSPRecomp -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
fi
echo "   ok  recompiler tools ready"

echo "== 5/6 Recompiling game from baserom.z64 =="
STAMP="build/recompile.stamp"
NEED_RECOMP=0
if [ ! -f "$STAMP" ] || [ ! -f "RecompiledFuncs/lookup.cpp" ] || [ ! -f "rsp/aspMain.cpp" ]; then
    NEED_RECOMP=1
else
    for f in baserom.z64 symbols.toml recomp.toml aspMain.toml "$N64RECOMP" "$RSPRECOMP" tools/*.py; do
        if [ "$f" -nt "$STAMP" ]; then
            NEED_RECOMP=1
            break
        fi
    done
fi

if [ "$NEED_RECOMP" -eq 1 ]; then
    rm -rf RecompiledFuncs
    "$N64RECOMP" recomp.toml
    python3 -I tools/fix_fallthroughs.py | tee build/fix_fallthroughs.log
    grep -q "chained to continuation:            143" build/fix_fallthroughs.log
    python3 -I tools/post_recomp_patches.py | tee build/post_recomp_patches.log
    grep -q "post-recomp redirects applied: 25" build/post_recomp_patches.log
    python3 -I tools/gen_lookup_table.py | tee build/gen_lookup_table.log
    grep -q "1409 function entries" build/gen_lookup_table.log
    mkdir -p rsp
    "$RSPRECOMP" aspMain.toml
    [ -f "rsp/aspMain.cpp" ] || { echo "ERROR: rsp/aspMain.cpp missing" >&2; exit 1; }
    date -u +"%Y-%m-%dT%H:%M:%SZ" > "$STAMP"
    echo "   ok  RecompiledFuncs/ and rsp/aspMain.cpp generated"
else
    echo "   ok  recompiled sources up to date"
fi

echo "== 6/6 Building Quest 3 APK via Gradle =="
chmod +x android/gradlew
(cd android && ./gradlew :app:assembleDebug --console=plain)

APK_PATH="android/app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK_PATH" ]; then
    echo "ERROR: APK was not produced at $APK_PATH" >&2
    exit 1
fi
echo "   ok  APK built: $APK_PATH"
