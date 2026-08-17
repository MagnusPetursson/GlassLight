#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/release-linux}"
OUTPUT_DIR="${2:-$ROOT/build/seed-gallery-0.2.0}"
VALIDATOR="$BUILD_DIR/glasslight-render-validation"

if [[ ! -x "$VALIDATOR" ]]; then
    echo "Missing $VALIDATOR; build the render-validation target first." >&2
    exit 2
fi

mkdir -p "$ROOT/build"
BUILD_ROOT="$(realpath "$ROOT/build")"
RESOLVED_OUTPUT="$(realpath -m "$OUTPUT_DIR")"
case "$RESOLVED_OUTPUT" in
    "$BUILD_ROOT"/*) ;;
    *)
        echo "Seed-gallery output must stay under $BUILD_ROOT (got $RESOLVED_OUTPUT)." >&2
        exit 2
        ;;
esac
mkdir -p "$RESOLVED_OUTPUT"

"$VALIDATOR" --gallery "$RESOLVED_OUTPUT"

echo "Gallery: $RESOLVED_OUTPUT"
echo "Manifest: $RESOLVED_OUTPUT/manifest.tsv"
