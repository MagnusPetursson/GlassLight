#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/release-linux}"
DIST_DIR="${2:-$ROOT/dist}"

if [[ ! -f "$BUILD_DIR/CPackConfig.cmake" ]]; then
    echo "Missing $BUILD_DIR/CPackConfig.cmake; configure the release-linux preset first." >&2
    exit 1
fi

mkdir -p "$DIST_DIR"
cpack --config "$BUILD_DIR/CPackConfig.cmake" -G DEB -B "$DIST_DIR"
