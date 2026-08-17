# GlassLight

GlassLight is a native, seed-driven generative art studio that traces light
through invisible procedural glass and renders only the resulting caustics on a
wall. Its Vulkan compute renderer supports live rotation, fixed-quality previews,
reproducible compositions, PNG settings metadata, and seamless H.264 MP4 loops.

## Shape families

GlassLight 0.2.0 includes six seeded glass families:

- **Pebble** — smooth, asymmetric organic volumes.
- **Lens** — broad optical forms with curved faces and a defined rim.
- **Ribbon** — twisted looping sections that cast long folded caustics.
- **Faceted Vessel** — hollow vessel-like silhouettes with broad planar facets.
- **Cut Crystal** — symmetric cut faces that break light into crisp clusters.
- **Fracture** — deterministic clusters of pointed convex crystals, available as
  rooted growths, exploded shards, or hybrids with seamless seeded motion.

The live canvas uses the selected Quality preset's fixed photon and interface
budget on the selected Vulkan GPU. On a slower device, update cadence may slow
or skip frames rather than silently lowering optical quality. Pausing allows a
stable preview; PNG and MP4 export render directly from the composition and
never capture the inspector or other UI chrome.

## Build

Requirements: CMake 3.24+, Ninja, a C++20 compiler, Vulkan 1.2 headers/loader,
`glslangValidator`, and a Vulkan-capable GPU. SDL 3, Dear ImGui, Vulkan Memory
Allocator, and JSON for Modern C++ are pinned and fetched during configure.

On Linux Mint or Ubuntu:

```sh
sudo apt install cmake ninja-build g++ libvulkan-dev vulkan-tools \
    glslang-tools ffmpeg
cmake --preset dev-linux
cmake --build --preset dev-linux
./build/dev-linux/glasslight
```

FFmpeg is optional at runtime and is required only for MP4 export. GlassLight
discovers it on `PATH`; still PNG export remains available without it.

## Use and verification

Choose **Generate** to materialize a seed, adjust the glass, palette, light, and
wall controls, then pause on a phase for stable inspection or export the current
phase. Exported PNG files carry the complete composition in a private `glAs`
chunk so they can restore their settings in GlassLight. Video export renders
one deterministic rotation and writes H.264/yuv420p MP4.
Composition schema v2 stores Fracture shard count, morphology, and motion under
`shape.fracture`; schema v1 images and sessions migrate to `9 / Auto / Auto`.

Useful non-interactive checks:

```sh
./build/dev-linux/glasslight --version
./build/dev-linux/glasslight --list-gpus
./build/dev-linux/glasslight --gpu-info
./build/dev-linux/glasslight --smoke-test build/smoke
./build/dev-linux/glasslight --video-smoke build/smoke/two-frame.mp4
ctest --preset dev-linux
```

GPU selection defaults to the highest-ranked compatible Vulkan device. To
override it, copy an exact name from `--list-gpus` into `GLASSLIGHT_GPU`, for
example `GLASSLIGHT_GPU="NVIDIA GeForce RTX 4070" ./build/dev-linux/glasslight`.

### GPU family validation and curated gallery

The optional render-validation executable checks every concrete family, preview
dimensions, same-device preview determinism, PNG settings restoration, photon
deposition, and UI-free wall output. It records tolerant structural image
statistics instead of comparing cross-driver pixels to a golden image:

```sh
cmake --build build/dev-linux --target glasslight-render-validation
./build/dev-linux/glasslight-render-validation \
    --validate build/render-validation-0.2.0
```

Create the reproducible review gallery (three fixed seeds for every family,
each as GPU preview and wall output) with:

```sh
bash tests/render_seed_gallery.sh build/release-linux \
    build/seed-gallery-0.2.0
```

The 36 PNGs and their `manifest.tsv` stay below `build/`; the script rejects a
tracked or external output location. Set `GLASSLIGHT_GPU` to compare the same
curated seeds on another compatible GPU.

The release performance gate is opt-in and should be run on the target Radeon,
not a software Vulkan device. It first warms the persistent pipelines, then
renders every family at Standard 1280×720 with exactly 1,843,200 photons. The
TSV records GPU and end-to-end timing. The five SDF families retain their 50 ms
gate; all nine forced 9-shard Fracture morphology/motion combinations have a
100 ms gate, while 16-shard variants are measured without a hard limit:

```sh
GLASSLIGHT_GPU="AMD Radeon 860M" \
    ./build/release-linux/glasslight-render-validation \
    --benchmark build/radeon-benchmark-0.2.0
```

The machine-readable result is `build/radeon-benchmark-0.2.0/benchmark.tsv`.
Use `--fracture OUTPUT_DIRECTORY` to render the full Fracture count/phase matrix
and its forced-plus-Auto contact sheet.

## Linux packages

Configure and build the release preset first. To create an AppImage, put
`linuxdeploy-x86_64.AppImage` on `PATH` (or set `LINUXDEPLOY`) and run:

```sh
cmake --preset release-linux
cmake --build --preset release-linux
bash packaging/linux/build-appimage.sh
```

To create the Debian package through CPack:

```sh
bash packaging/linux/build-deb.sh
```

The package metadata and desktop integration are relocatable and contain no
machine-specific absolute paths.

Before publishing 0.2.0, validate a staged install without touching the active
desktop launcher:

```sh
DESTDIR="$PWD/build/install-check" cmake --install build/release-linux --prefix /usr
bash packaging/linux/verify-install-tree.sh build/install-check
desktop-file-validate packaging/linux/is.magnusp.glasslight.desktop
appstreamcli validate --no-net packaging/linux/is.magnusp.glasslight.appdata.xml
```

## Current platform scope

Version 0.2 targets Linux x86_64. The renderer and media interfaces are kept
portable for a later Windows build, but Windows packaging and a CPU/OpenGL
fallback are not included yet.

## License

GlassLight is MIT licensed. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
