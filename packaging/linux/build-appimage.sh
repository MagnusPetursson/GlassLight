#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/release-linux}"
DIST_DIR="${2:-$ROOT/dist}"
LINUXDEPLOY="${LINUXDEPLOY:-linuxdeploy-x86_64.AppImage}"
APPDIR="$BUILD_DIR/AppDir"
VERSION="${GLASSLIGHT_VERSION:-$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")}"

if [[ -z "$VERSION" ]]; then
    echo "Could not determine GlassLight version from $BUILD_DIR/CMakeCache.txt" >&2
    exit 1
fi

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr --strip

mkdir -p "$DIST_DIR"
LDAI_OUTPUT="$DIST_DIR/GlassLight-$VERSION-x86_64.AppImage" \
LDAI_NO_APPSTREAM=1 \
APPIMAGE_EXTRACT_AND_RUN=1 "$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --output appimage
