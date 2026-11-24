#!/usr/bin/env bash
# build.sh — Build ArchDiView.wlx64 (64-bit) and ArchDiView.wlx (32-bit).
# Pure C++/GDI, so no third-party deps are needed (unlike the WebView plugin).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

GEN="${1:-}"
BUILD64="$ROOT/build/release"
BUILD32="$ROOT/build/release32"

configure() {
    local dir="$1" arch="$2"
    if [ ! -f "$dir/CMakeCache.txt" ]; then
        echo "==> Configuring $arch in $dir"
        cmake -S . -B "$dir" -G "Visual Studio 17 2022" -A "$arch" \
              -DCMAKE_BUILD_TYPE=Release
    fi
}

build() {
    local dir="$1" label="$2"
    echo "==> Building $label"
    cmake --build "$dir" --config Release
}

# Configure + build 64-bit and 32-bit.
configure "$BUILD64" x64
build    "$BUILD64" "64-bit (ArchDiView.wlx64)"
configure "$BUILD32" Win32
build    "$BUILD32" "32-bit (ArchDiView.wlx)"

echo ""
echo "==> Done."
ls -l "$BUILD64/Release/ArchDiView.wlx64" "$BUILD32/Release/ArchDiView.wlx" 2>/dev/null || true
