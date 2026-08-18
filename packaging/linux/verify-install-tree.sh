#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 INSTALL_ROOT" >&2
    echo "INSTALL_ROOT may be an AppDir/DESTDIR containing usr/, or an install prefix." >&2
    exit 2
fi

INSTALL_ROOT="$(realpath "$1")"
EXPECTED_VERSION="${GLASSLIGHT_VERSION:-0.2.0}"
if [[ -d "$INSTALL_ROOT/usr" ]]; then
    PREFIX="$INSTALL_ROOT/usr"
else
    PREFIX="$INSTALL_ROOT"
fi

BINARY="$PREFIX/bin/glasslight"
DESKTOP="$PREFIX/share/applications/is.magnusp.glasslight.desktop"
ICON="$PREFIX/share/icons/hicolor/scalable/apps/is.magnusp.glasslight.svg"
APPSTREAM="$PREFIX/share/metainfo/is.magnusp.glasslight.appdata.xml"
SHADER_DIR="$PREFIX/share/glasslight/shaders"

for path in "$BINARY" "$DESKTOP" "$ICON" "$APPSTREAM" \
            "$SHADER_DIR/first_light.comp.spv" "$SHADER_DIR/glass_preview.comp.spv" \
            "$SHADER_DIR/first_light.comp.fracture.spv" \
            "$SHADER_DIR/glass_preview.comp.fracture.spv" \
            "$SHADER_DIR/first_light.comp.group128.fracture.spv" \
            "$SHADER_DIR/glass_preview.comp.group8.fracture.spv" \
            "$SHADER_DIR/resolve.comp.spv" "$SHADER_DIR/resolve.comp.group8.spv"; do
    if [[ ! -e "$path" ]]; then
        echo "Missing installed artifact: $path" >&2
        exit 1
    fi
done
for family in 1 2 3 4 5; do
    for shader in first_light.comp glass_preview.comp; do
        path="$SHADER_DIR/$shader.family$family.spv"
        if [[ ! -f "$path" ]]; then
            echo "Missing installed specialized shader: $path" >&2
            exit 1
        fi
    done
    for shader in "first_light.comp.group128.family$family.spv" \
                  "glass_preview.comp.group8.family$family.spv"; do
        path="$SHADER_DIR/$shader"
        if [[ ! -f "$path" ]]; then
            echo "Missing installed workgroup candidate shader: $path" >&2
            exit 1
        fi
    done
done
if [[ ! -x "$BINARY" ]]; then
    echo "Installed GlassLight binary is not executable: $BINARY" >&2
    exit 1
fi

desktop-file-validate "$DESKTOP"
appstreamcli validate --no-net "$APPSTREAM"
grep -qx 'Exec=glasslight' "$DESKTOP"
grep -qx 'Icon=is.magnusp.glasslight' "$DESKTOP"
grep -q '<launchable type="desktop-id">is.magnusp.glasslight.desktop</launchable>' \
    "$APPSTREAM"
grep -q '<binary>glasslight</binary>' "$APPSTREAM"
grep -q "<release version=\"$EXPECTED_VERSION\"" "$APPSTREAM"

if grep -R -n -E '/home/[^/]+/|/tmp/' "$DESKTOP" "$APPSTREAM" "$ICON"; then
    echo "Installed desktop metadata contains a machine-local absolute path." >&2
    exit 1
fi

echo "Install tree metadata and launcher contract are valid: $PREFIX"
