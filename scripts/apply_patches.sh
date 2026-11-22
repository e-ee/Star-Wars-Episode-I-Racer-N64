#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

apply_one() {
    local target_dir="$1"
    local patch_file="$2"
    if [ ! -f "$patch_file" ]; then
        return 0
    fi
    if git -C "$target_dir" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        echo "[patches] already applied: $(basename "$patch_file")"
        return 0
    fi
    echo "[patches] applying $(basename "$patch_file") -> $target_dir"
    git -C "$target_dir" apply "$patch_file"
}

apply_one "$ROOT_DIR/lib/rt64" "$ROOT_DIR/patches/rt64-macos-android-build.patch"
apply_one "$ROOT_DIR/lib/rt64" "$ROOT_DIR/patches/rt64-openxr-stereo.patch"
apply_one "$ROOT_DIR/lib/rt64/src/contrib/plume" "$ROOT_DIR/patches/plume-openxr-vulkan.patch"
