# GlassLight development

This document covers the contributor, validation, and release workflows kept
out of the public-facing README.

## Toolchain and builds

GlassLight requires CMake 3.24+, Ninja, a C++20 compiler, Vulkan 1.2 headers
and loader, and `glslangValidator`. FFmpeg is optional at runtime and is used
only for MP4 export.

SDL 3, Dear ImGui, Vulkan Memory Allocator, and JSON for Modern C++ are pinned
with CMake FetchContent and downloaded during configuration.

On Linux Mint or Ubuntu:

```sh
sudo apt install cmake ninja-build g++ libvulkan-dev vulkan-tools \
    glslang-tools ffmpeg
```

Configure and build a development tree:

```sh
cmake --preset dev-linux
cmake --build --preset dev-linux
```

For optimized binaries and packages, use the release preset:

```sh
cmake --preset release-linux
cmake --build --preset release-linux
```

Both presets enable the test targets. Their build directories live below
`build/` and are ignored by Git.

On Windows 10 or 11 x64, install Visual Studio 2022 Build Tools with the C++
workload, Ninja, CMake, and the LunarG Vulkan SDK. Open an x64 Native Tools
terminal and run:

```powershell
cmake --preset release-windows
cmake --build --preset release-windows
ctest --preset release-windows
```

The Vulkan SDK is a build dependency and supplies headers, the import library,
and `glslangValidator`. End users need only the Vulkan runtime installed by a
current GPU driver.

## Project layout

- `src/core/` owns compositions, deterministic seeds, validation, persistence,
  and Fracture layouts.
- `src/render/` and `shaders/` implement the Vulkan compute renderer and the
  separate glass-object preview.
- `src/studio/` and `src/app/` contain the ImGui interface, SDL application,
  dialogs, export coordination, and CLI entry point.
- `src/media/` implements PNG composition metadata and FFmpeg-backed MP4
  export; `src/platform/` owns UTF-8 paths, durable writes, and native replace
  semantics.
- `tests/` contains CPU tests, GPU structural validation, benchmarks, and the
  curated seed gallery. `packaging/` contains Linux integration and Windows
  resources/package verification.

## CLI and GPU selection

The release and development binaries expose the same commands:

```text
glasslight                         Open the native studio
glasslight --version
glasslight --render-still FILE [SEED] SHAPE
glasslight --smoke-test DIRECTORY
glasslight --video-smoke FILE
glasslight --list-gpus
glasslight --gpu-info
```

Valid `SHAPE` values are `auto`, `pebble`, `lens`, `ribbon`,
`faceted-vessel`, `cut-crystal`, and `fracture`. For example:

```sh
./build/dev-linux/glasslight --render-still build/example.png 123 cut-crystal
```

GlassLight chooses the highest-ranked compatible Vulkan device by default.
Copy an exact name from `--list-gpus` into `GLASSLIGHT_GPU` to override it:

```sh
GLASSLIGHT_GPU="AMD Radeon 860M Graphics (RADV GFX1152)" \
    ./build/dev-linux/glasslight --gpu-info
```

The selected Quality preset fixes the photon and interface budget. Slower
devices may update less often instead of silently reducing optical quality.

## Tests and renderer validation

Run the CPU-side suite through its preset:

```sh
ctest --preset dev-linux
```

The suite covers composition validation and migration, persistent session
state, PNG metadata, studio state, and the FFmpeg argument and process
contract. The video tests exercise FFmpeg when it is available.

GPU checks are intentionally separate from CTest so headless builders without
Vulkan remain usable. Build and run the validation executable explicitly:

```sh
cmake --build build/dev-linux --target glasslight-render-validation
./build/dev-linux/glasslight-render-validation \
    --validate build/render-validation-0.2.0
```

Validation covers every concrete family, preview dimensions, same-device
determinism, PNG settings restoration, photon deposition, and UI-free wall
output. It compares tolerant structural image statistics rather than requiring
identical pixels across drivers.

Create the curated review gallery with three fixed seeds for every family:

```sh
bash tests/render_seed_gallery.sh build/release-linux \
    build/seed-gallery-0.2.0
```

The script writes its PNGs and `manifest.tsv` below `build/` and rejects
tracked or external output paths. Use `GLASSLIGHT_GPU` to render the same seeds
on another compatible device.

## Performance and Fracture review

Run performance gates on the target physical GPU, not a software Vulkan
device. The benchmark warms persistent pipelines, then renders every family at
Standard 1280×720 with exactly 1,843,200 photons:

```sh
GLASSLIGHT_GPU="AMD Radeon 860M Graphics (RADV GFX1152)" \
    ./build/release-linux/glasslight-render-validation \
    --benchmark build/radeon-benchmark-0.2.0
```

The five SDF families have a 50 ms gate. The nine forced 9-shard Fracture
morphology and motion combinations have a 100 ms gate; 16-shard variants are
measured without a hard limit. Results are written to `benchmark.tsv` in the
selected output directory.

Use the Fracture mode to render its full shard-count and phase matrix plus the
forced-versus-Auto contact sheet:

```sh
./build/release-linux/glasslight-render-validation \
    --fracture build/fracture-review-0.2.0
```

## Composition and export compatibility

Still exports are rendered directly from the composition and never capture UI
chrome. PNG files store the complete serialized composition in the private,
ancillary `glAs` chunk. Files without that chunk remain valid PNGs but cannot
restore GlassLight settings.

MP4 export renders one deterministic rotation and passes raw RGBA frames to an
external FFmpeg process for H.264/yuv420p output. The serialized composition is
stored in versioned comment metadata.

Composition schema v2 stores Fracture shard count, morphology, and motion below
`shape.fracture`. Schema v1 sessions and PNGs migrate to `9 / Auto / Auto`.
On Linux, the last session is stored at
`$XDG_CONFIG_HOME/glasslight/settings.json`, or
`~/.config/glasslight/settings.json` when `XDG_CONFIG_HOME` is unset. Windows
uses `%APPDATA%\GlassLight\settings.json`.

## Linux packages

Build the release preset before packaging. To create an AppImage, place
`linuxdeploy-x86_64.AppImage` on `PATH` or set `LINUXDEPLOY`, then run:

```sh
bash packaging/linux/build-appimage.sh build/release-linux dist
```

Create the Debian package through CPack:

```sh
bash packaging/linux/build-deb.sh build/release-linux dist
```

Validate a staged install without touching the active desktop launcher:

```sh
DESTDIR="$PWD/build/install-check" \
    cmake --install build/release-linux --prefix /usr
bash packaging/linux/verify-install-tree.sh build/install-check
desktop-file-validate packaging/linux/is.magnusp.glasslight.desktop
appstreamcli validate --no-net \
    packaging/linux/is.magnusp.glasslight.appdata.xml
```

The package metadata and desktop integration must remain relocatable and free
of machine-specific absolute paths.

## Windows package

Build and test the release preset, then create the portable ZIP:

```powershell
cpack --config build/release-windows/CPackConfig.cmake -G ZIP -B dist
./packaging/windows/verify-package.ps1 `
    -Archive dist/GlassLight-0.2.0-windows-x64.zip
```

The archive contains only `GlassLight.exe`, the five compiled shaders, and the
README/license notices. It does not contain FFmpeg, the Vulkan SDK or runtime,
demo exports, screenshots, or an installer. MP4 export searches first for
`ffmpeg.exe` beside the application and then on `PATH`.

GitHub Actions builds this ZIP natively with MSVC on every push and pull
request. Before tagging a release, download that artifact and validate launch,
high-DPI behavior, every glass family, Unicode PNG save/restore, AppData state,
and MP4 export on a physical Windows Vulkan 1.2 GPU. Hosted CI validates the
build and CPU tests but is not a physical-GPU rendering gate.

Tags matching `v*` must equal the CMake project version. A `v0.2.0` tag builds
the AppImage, DEB, and Windows ZIP, generates one `SHA256SUMS`, and publishes a
normal GitHub release.
