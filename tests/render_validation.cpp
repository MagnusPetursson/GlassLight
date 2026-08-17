#include "core/composition.hpp"
#include "media/png_metadata.hpp"
#include "render/vulkan_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::array kFamilies{
    glasslight::ShapeFamily::Pebble,
    glasslight::ShapeFamily::Lens,
    glasslight::ShapeFamily::Ribbon,
    glasslight::ShapeFamily::FacetedVessel,
    glasslight::ShapeFamily::CutCrystal,
    glasslight::ShapeFamily::Fracture};

constexpr std::array<std::uint64_t, 3> kGallerySeeds{
    0x18a4f0397c2d6b51ULL,
    0x73e9b124ad805fc6ULL,
    0xd4c26791e83ab50fULL};

struct ImageStructure {
    double meanLuminance = 0.0;
    double luminanceDeviation = 0.0;
    double meanChroma = 0.0;
    double colorfulFraction = 0.0;
    double brightFraction = 0.0;
    double colorCentroidX = 0.5;
    double colorCentroidY = 0.5;
    std::uint64_t opaquePixels = 0;
    std::uint64_t hash = 1469598103934665603ULL;
};

std::string familySlug(const glasslight::ShapeFamily family) {
    switch (family) {
    case glasslight::ShapeFamily::Pebble: return "pebble";
    case glasslight::ShapeFamily::Lens: return "lens";
    case glasslight::ShapeFamily::Ribbon: return "ribbon";
    case glasslight::ShapeFamily::FacetedVessel: return "faceted-vessel";
    case glasslight::ShapeFamily::CutCrystal: return "cut-crystal";
    case glasslight::ShapeFamily::Fracture: return "fracture";
    case glasslight::ShapeFamily::Auto: return "auto";
    }
    return "unknown";
}

std::string wallSlug(const glasslight::WallPreset wall) {
    switch (wall) {
    case glasslight::WallPreset::WarmWhitePaint: return "warm-white";
    case glasslight::WallPreset::NeutralGallery: return "neutral-gallery";
    case glasslight::WallPreset::CoolPlaster: return "cool-plaster";
    case glasslight::WallPreset::Charcoal: return "charcoal";
    }
    return "unknown-wall";
}

std::string morphologySlug(const glasslight::FractureMorphology morphology) {
    switch (morphology) {
    case glasslight::FractureMorphology::Growth: return "growth";
    case glasslight::FractureMorphology::Exploded: return "exploded";
    case glasslight::FractureMorphology::Hybrid: return "hybrid";
    case glasslight::FractureMorphology::Auto: return "auto";
    }
    return "unknown";
}

std::string motionSlug(const glasslight::FractureMotion motion) {
    switch (motion) {
    case glasslight::FractureMotion::Rigid: return "rigid";
    case glasslight::FractureMotion::GrowAndRecede: return "grow-recede";
    case glasslight::FractureMotion::BreakAndReform: return "break-reform";
    case glasslight::FractureMotion::Auto: return "auto";
    }
    return "unknown";
}

std::string tsvSafe(std::string value) {
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

ImageStructure analyze(const glasslight::RenderedImage& image) {
    ImageStructure result;
    if (image.width == 0u || image.height == 0u || image.rgba8.empty()) {
        return result;
    }

    long double luminanceSum = 0.0;
    long double luminanceSquaredSum = 0.0;
    long double chromaSum = 0.0;
    long double centroidWeight = 0.0;
    long double centroidX = 0.0;
    long double centroidY = 0.0;
    std::uint64_t colorfulPixels = 0;
    std::uint64_t brightPixels = 0;
    for (std::size_t index = 0; index < image.rgba8.size(); ++index) {
        const std::uint32_t packed = image.rgba8[index];
        const double red = static_cast<double>(packed & 0xffu) / 255.0;
        const double green = static_cast<double>((packed >> 8u) & 0xffu) / 255.0;
        const double blue = static_cast<double>((packed >> 16u) & 0xffu) / 255.0;
        const std::uint32_t alpha = (packed >> 24u) & 0xffu;
        result.opaquePixels += alpha == 0xffu ? 1u : 0u;
        const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
        const double maximum = std::max({red, green, blue});
        const double minimum = std::min({red, green, blue});
        const double chroma = maximum - minimum;
        luminanceSum += luminance;
        luminanceSquaredSum += luminance * luminance;
        chromaSum += chroma;
        colorfulPixels += chroma > 0.035 ? 1u : 0u;
        brightPixels += luminance > 0.72 ? 1u : 0u;
        const double weight = chroma * chroma;
        centroidWeight += weight;
        centroidX += weight *
            (static_cast<double>(index % image.width) + 0.5) / image.width;
        centroidY += weight *
            (static_cast<double>(index / image.width) + 0.5) / image.height;

        for (unsigned int shift = 0; shift < 32u; shift += 8u) {
            result.hash ^= (packed >> shift) & 0xffu;
            result.hash *= 1099511628211ULL;
        }
    }

    const long double count = static_cast<long double>(image.rgba8.size());
    result.meanLuminance = static_cast<double>(luminanceSum / count);
    const long double variance = std::max(
        0.0L, luminanceSquaredSum / count -
                  (luminanceSum / count) * (luminanceSum / count));
    result.luminanceDeviation = std::sqrt(static_cast<double>(variance));
    result.meanChroma = static_cast<double>(chromaSum / count);
    result.colorfulFraction = static_cast<double>(colorfulPixels) /
                              static_cast<double>(image.rgba8.size());
    result.brightFraction = static_cast<double>(brightPixels) /
                            static_cast<double>(image.rgba8.size());
    if (centroidWeight > 1.0e-12L) {
        result.colorCentroidX = static_cast<double>(centroidX / centroidWeight);
        result.colorCentroidY = static_cast<double>(centroidY / centroidWeight);
    }
    return result;
}

bool structurallyUseful(const glasslight::RenderedImage& image,
                        const glasslight::RenderStats& stats,
                        const ImageStructure& structure,
                        std::string& reason) {
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(image.width) * image.height;
    if (pixelCount == 0u || image.rgba8.size() != pixelCount) {
        reason = "pixel count does not match the requested dimensions";
        return false;
    }
    if (structure.opaquePixels != pixelCount) {
        reason = "renderer output contains non-opaque pixels";
        return false;
    }
    if (image.nonBackgroundPixels <= pixelCount / 100u) {
        reason = "fewer than one percent of pixels differ from the wall background";
        return false;
    }
    if (stats.depositedSamples == 0u || stats.estimatedHitRate <= 0.0 ||
        stats.estimatedHitRate > 1.0) {
        reason = "photon deposition statistics are outside the useful range";
        return false;
    }
    if (structure.luminanceDeviation < 0.002 ||
        structure.colorfulFraction < 0.0002 || structure.meanChroma < 0.0001) {
        reason = "image lacks the luminance or color variation expected from caustics";
        return false;
    }
    return true;
}

bool previewStructurallyUseful(const glasslight::RenderedImage& image,
                               const glasslight::GlassPreviewStats& stats,
                               const ImageStructure& structure,
                               std::string& reason) {
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(image.width) * image.height;
    if (pixelCount == 0u || image.rgba8.size() != pixelCount) {
        reason = "glass-preview pixel count does not match the requested dimensions";
        return false;
    }
    if (structure.opaquePixels != pixelCount) {
        reason = "glass preview contains non-opaque pixels";
        return false;
    }
    if (stats.shadedPixels <= pixelCount / 100u ||
        image.nonBackgroundPixels <= pixelCount / 100u) {
        reason = "glass preview shaded fewer than one percent of its pixels";
        return false;
    }
    if (structure.luminanceDeviation < 0.01 ||
        structure.colorfulFraction < 0.001 || structure.meanChroma < 0.001) {
        reason = "glass preview lacks useful form or color variation";
        return false;
    }
    return true;
}

// This deliberately compares broad, resolution-independent properties rather
// than pixels. It is suitable for Vulkan output from different conformant
// drivers, where floating-point details may differ while the composition is
// still structurally equivalent.
bool structurallySimilar(const ImageStructure& first,
                         const ImageStructure& second,
                         std::string& reason) {
    const auto within = [](const double a, const double b, const double tolerance) {
        return std::abs(a - b) <= tolerance;
    };
    if (!within(first.meanLuminance, second.meanLuminance, 0.08)) {
        reason = "mean luminance differs by more than 0.08";
    } else if (!within(first.luminanceDeviation, second.luminanceDeviation, 0.10)) {
        reason = "luminance deviation differs by more than 0.10";
    } else if (!within(first.meanChroma, second.meanChroma, 0.08)) {
        reason = "mean chroma differs by more than 0.08";
    } else if (!within(first.colorfulFraction, second.colorfulFraction, 0.20)) {
        reason = "colorful-pixel coverage differs by more than 0.20";
    } else if (!within(first.brightFraction, second.brightFraction, 0.20)) {
        reason = "bright-pixel coverage differs by more than 0.20";
    } else if (!within(first.colorCentroidX, second.colorCentroidX, 0.16) ||
               !within(first.colorCentroidY, second.colorCentroidY, 0.16)) {
        reason = "colored-light centroid moved by more than 16 percent of the frame";
    } else {
        return true;
    }
    return false;
}

glasslight::media::RgbaImage asMediaImage(const glasslight::RenderedImage& image) {
    glasslight::media::RgbaImage mediaImage;
    mediaImage.width = image.width;
    mediaImage.height = image.height;
    mediaImage.pixels.resize(image.rgba8.size() * sizeof(std::uint32_t));
    if (!mediaImage.pixels.empty()) {
        std::memcpy(mediaImage.pixels.data(), image.rgba8.data(),
                    mediaImage.pixels.size());
    }
    return mediaImage;
}

bool render(glasslight::VulkanRenderer& renderer,
            const glasslight::CompositionSettings& settings,
            const std::uint32_t width, const std::uint32_t height,
            const std::uint32_t samples, const bool exportRender,
            glasslight::RenderedImage& image, glasslight::RenderStats& stats,
            std::string& error) {
    glasslight::RenderRequest request;
    request.settings = settings;
    request.width = width;
    request.height = height;
    request.samples = samples;
    request.exportRender = exportRender;
    return renderer.render(request, image, stats, error);
}

bool renderGlassPreview(glasslight::VulkanRenderer& renderer,
                        const glasslight::CompositionSettings& settings,
                        const std::uint32_t width, const std::uint32_t height,
                        glasslight::RenderedImage& image,
                        glasslight::GlassPreviewStats& stats,
                        std::string& error) {
    glasslight::GlassPreviewRequest request;
    request.settings = settings;
    request.width = width;
    request.height = height;
    request.orbitYaw = 0.72f;
    request.orbitPitch = 0.31f;
    request.zoom = 1.0f;
    request.edgeOverlay = true;
    return renderer.renderGlassPreview(request, image, stats, error);
}

glasslight::RenderStats asReportStats(const glasslight::GlassPreviewStats& preview,
                                      const glasslight::RenderedImage& image) {
    glasslight::RenderStats stats;
    stats.resolvedFamily = preview.resolvedFamily;
    stats.depositedSamples = preview.shadedPixels;
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(image.width) * image.height;
    stats.estimatedHitRate = pixelCount == 0u ? 0.0 :
        static_cast<double>(preview.shadedPixels) / static_cast<double>(pixelCount);
    return stats;
}

void writeReportLine(std::ofstream& report, const std::string& file,
                     const glasslight::CompositionSettings& settings,
                     const std::string& mode,
                     const glasslight::RenderedImage& image,
                     const glasslight::RenderStats& stats,
                     const ImageStructure& structure) {
    report << file << '\t' << familySlug(stats.resolvedFamily) << '\t'
           << settings.seed << '\t' << mode << '\t' << wallSlug(settings.wall) << '\t'
           << image.width << '\t' << image.height << '\t' << std::hex << structure.hash
           << std::dec << '\t' << std::fixed << std::setprecision(6)
           << structure.meanLuminance << '\t' << structure.luminanceDeviation << '\t'
           << structure.meanChroma << '\t' << structure.colorfulFraction << '\t'
           << structure.brightFraction << '\t' << structure.colorCentroidX << '\t'
           << structure.colorCentroidY << '\t' << stats.depositedSamples << '\t'
           << stats.estimatedHitRate << '\t' << image.renderMilliseconds << '\t'
           << tsvSafe(image.gpuName) << '\n';
}

void writeReportHeader(std::ofstream& report) {
    report << "file\tfamily\tseed\tmode\twall\twidth\theight\tfnv1a64"
              "\tmean_luma\tluma_deviation\tmean_chroma\tcolorful_fraction"
              "\tbright_fraction\tcolor_centroid_x\tcolor_centroid_y\tdeposits"
              "\thit_rate\trender_ms\tgpu\n";
}

bool saveResult(const std::filesystem::path& path,
                const glasslight::CompositionSettings& settings,
                const glasslight::RenderedImage& image,
                std::string& error) {
    const std::string serialized = glasslight::serializeSettings(settings);
    if (!glasslight::media::savePngWithSettings(
            path, asMediaImage(image), serialized, error)) {
        return false;
    }
    glasslight::media::RgbaImage restored;
    std::string restoredSettings;
    if (!glasslight::media::loadPngWithSettings(
            path, restored, restoredSettings, error)) {
        return false;
    }
    if (restored.width != image.width || restored.height != image.height ||
        restoredSettings != serialized) {
        error = "rendered PNG dimensions or embedded settings did not round-trip";
        return false;
    }
    return true;
}

glasslight::RenderedImage makeContactSheet(
        const std::vector<glasslight::RenderedImage>& tiles,
        const std::uint32_t columns) {
    glasslight::RenderedImage sheet;
    if (tiles.empty() || columns == 0u) return sheet;
    const std::uint32_t tileWidth = tiles.front().width;
    const std::uint32_t tileHeight = tiles.front().height;
    const std::uint32_t rows = static_cast<std::uint32_t>(
        (tiles.size() + columns - 1u) / columns);
    sheet.width = tileWidth * columns;
    sheet.height = tileHeight * rows;
    sheet.gpuName = tiles.front().gpuName;
    sheet.rgba8.assign(static_cast<std::size_t>(sheet.width) * sheet.height, 0xff101820u);
    for (std::size_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex) {
        const auto& tile = tiles[tileIndex];
        const std::uint32_t tileX = static_cast<std::uint32_t>(tileIndex % columns) * tileWidth;
        const std::uint32_t tileY = static_cast<std::uint32_t>(tileIndex / columns) * tileHeight;
        for (std::uint32_t y = 0; y < tileHeight; ++y) {
            std::copy_n(tile.rgba8.begin() + static_cast<std::size_t>(y) * tileWidth,
                        tileWidth,
                        sheet.rgba8.begin() + static_cast<std::size_t>(tileY + y) *
                            sheet.width + tileX);
        }
    }
    sheet.nonBackgroundPixels = sheet.rgba8.size();
    return sheet;
}

int validateFracture(glasslight::VulkanRenderer& renderer,
                     const std::filesystem::path& outputDirectory) {
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create Fracture validation directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }
    std::ofstream report(outputDirectory / "fracture.tsv", std::ios::trunc);
    if (!report) return 2;
    writeReportHeader(report);
    int failures = 0;
    const auto fail = [&](const std::string& message) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    };
    constexpr std::array morphologies{
        glasslight::FractureMorphology::Growth,
        glasslight::FractureMorphology::Exploded,
        glasslight::FractureMorphology::Hybrid};
    constexpr std::array motions{
        glasslight::FractureMotion::Rigid,
        glasslight::FractureMotion::GrowAndRecede,
        glasslight::FractureMotion::BreakAndReform};
    constexpr std::array counts{3u, 9u, 16u};
    constexpr std::array phases{0.0f, 0.33333334f, 0.66666669f};
    std::vector<glasslight::RenderedImage> contactTiles;
    std::string error;

    for (const auto morphology : morphologies) {
        for (const auto motion : motions) {
            for (const std::uint32_t count : counts) {
                for (const float phase : phases) {
                    glasslight::CompositionSettings settings;
                    settings.seed = 0xf123000000000000ULL +
                        static_cast<std::uint64_t>(morphology) * 0x10001u +
                        static_cast<std::uint64_t>(motion) * 0x101u + count;
                    settings.shapeFamily = glasslight::ShapeFamily::Fracture;
                    settings.fractureShardCount = count;
                    settings.fractureMorphology = morphology;
                    settings.fractureMotion = motion;
                    settings.phase = phase;
                    settings.quality = glasslight::QualityPreset::Draft;
                    glasslight::RenderedImage preview;
                    glasslight::GlassPreviewStats previewStats;
                    if (!renderGlassPreview(renderer, settings, 192, 192,
                                            preview, previewStats, error)) {
                        fail(morphologySlug(morphology) + "/" + motionSlug(motion) +
                             " preview failed: " + error);
                        continue;
                    }
                    const ImageStructure structure = analyze(preview);
                    std::string reason;
                    if (!previewStructurallyUseful(preview, previewStats, structure, reason)) {
                        fail(morphologySlug(morphology) + "/" + motionSlug(motion) +
                             " preview was empty: " + reason);
                    }
                    const std::string stem = morphologySlug(morphology) + "-" +
                        motionSlug(motion) + "-c" + std::to_string(count) + "-p" +
                        std::to_string(static_cast<int>(phase * 100.0f));
                    if (!saveResult(outputDirectory / (stem + "-preview.png"),
                                    settings, preview, error)) {
                        fail(stem + " PNG metadata restoration failed: " + error);
                    }
                    writeReportLine(report, stem + "-preview.png", settings,
                                    "glass-preview", preview,
                                    asReportStats(previewStats, preview), structure);

                    if (count == 9u && phase == phases[1]) {
                        contactTiles.push_back(preview);
                        glasslight::RenderedImage repeat;
                        glasslight::GlassPreviewStats repeatStats;
                        if (!renderGlassPreview(renderer, settings, 192, 192,
                                                repeat, repeatStats, error)) {
                            fail(stem + " deterministic repeat failed: " + error);
                        } else {
                            if (analyze(repeat).hash != structure.hash) {
                                fail(stem + " was not same-device deterministic");
                            }
                        }
                    }
                }
            }

            for (const float phase : {0.0f, 0.5f}) {
                glasslight::CompositionSettings settings;
                settings.seed = 0xfa11000000000000ULL +
                    static_cast<std::uint64_t>(morphology) * 0x100u +
                    static_cast<std::uint64_t>(motion);
                settings.shapeFamily = glasslight::ShapeFamily::Fracture;
                settings.fractureShardCount = 9;
                settings.fractureMorphology = morphology;
                settings.fractureMotion = motion;
                settings.phase = phase;
                settings.quality = glasslight::QualityPreset::Draft;
                glasslight::RenderedImage wall;
                glasslight::RenderStats stats;
                if (!render(renderer, settings, 240, 135, 65'536u, true,
                            wall, stats, error)) {
                    fail(morphologySlug(morphology) + "/" + motionSlug(motion) +
                         " wall failed: " + error);
                    continue;
                }
                const ImageStructure structure = analyze(wall);
                std::string reason;
                if (!structurallyUseful(wall, stats, structure, reason)) {
                    fail(morphologySlug(morphology) + "/" + motionSlug(motion) +
                         " wall was empty: " + reason);
                }
                if (stats.interfaceEvents == 0u ||
                    stats.traversalExhausted > stats.tracedSamples / 2u) {
                    fail(morphologySlug(morphology) + "/" + motionSlug(motion) +
                         " produced invalid interfaces or excessive traversal exhaustion");
                }
                const std::string stem = morphologySlug(morphology) + "-" +
                    motionSlug(motion) + "-wall-p" +
                    std::to_string(static_cast<int>(phase * 100.0f));
                if (!saveResult(outputDirectory / (stem + ".png"), settings, wall, error)) {
                    fail(stem + " PNG metadata restoration failed: " + error);
                }
                writeReportLine(report, stem + ".png", settings, "wall", wall,
                                stats, structure);
            }
        }
    }

    for (std::uint64_t seed = 1u; seed <= 12u; ++seed) {
        glasslight::CompositionSettings settings;
        settings.seed = 0xa170000000000000ULL + seed * 0x9e37u;
        settings.shapeFamily = glasslight::ShapeFamily::Fracture;
        settings.fractureShardCount = 9;
        settings.fractureMorphology = glasslight::FractureMorphology::Auto;
        settings.fractureMotion = glasslight::FractureMotion::Auto;
        settings.phase = 0.37f;
        settings.quality = glasslight::QualityPreset::Draft;
        glasslight::RenderedImage preview;
        glasslight::GlassPreviewStats stats;
        if (!renderGlassPreview(renderer, settings, 192, 192, preview, stats, error)) {
            fail("Auto seed " + std::to_string(seed) + " failed: " + error);
        } else {
            contactTiles.push_back(std::move(preview));
        }
    }
    if (contactTiles.size() == 21u) {
        const glasslight::RenderedImage sheet = makeContactSheet(contactTiles, 7u);
        glasslight::CompositionSettings metadata;
        metadata.seed = 0x4652414354555245ULL;
        metadata.shapeFamily = glasslight::ShapeFamily::Fracture;
        if (!saveResult(outputDirectory / "fracture-contact-sheet.png",
                        metadata, sheet, error)) {
            fail("contact sheet save failed: " + error);
        }
    } else {
        fail("contact sheet did not collect all nine forced and twelve Auto tiles");
    }
    report.flush();
    if (failures == 0) {
        std::cout << "Fracture morphology, motion, count, phase, PNG, and determinism "
                     "validation passed on " << renderer.gpuName() << ".\n";
    }
    return failures == 0 ? 0 : 1;
}

int validateFamilies(glasslight::VulkanRenderer& renderer,
                     const std::filesystem::path& outputDirectory) {
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create validation directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }
    std::ofstream report(outputDirectory / "structure.tsv", std::ios::trunc);
    if (!report) {
        std::cerr << "Could not create validation report.\n";
        return 2;
    }
    writeReportHeader(report);

    int failures = 0;
    const auto fail = [&](const std::string& message) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    };

    // Pick an Auto seed that resolves to one of the 0.2 families, ensuring the
    // core selection and renderer dispatch stay in lock-step as the enum grows.
    glasslight::CompositionSettings automatic;
    automatic.shapeFamily = glasslight::ShapeFamily::Auto;
    automatic.quality = glasslight::QualityPreset::Draft;
    for (automatic.seed = 1u; automatic.seed < 100'000u; ++automatic.seed) {
        if (static_cast<std::uint32_t>(glasslight::resolveShapeFamily(automatic)) >=
            static_cast<std::uint32_t>(glasslight::ShapeFamily::FacetedVessel)) {
            break;
        }
    }
    glasslight::RenderedImage automaticImage;
    glasslight::RenderedImage automaticGlassPreview;
    glasslight::RenderStats automaticStats;
    glasslight::GlassPreviewStats automaticGlassStats;
    std::string error;
    if (!render(renderer, automatic, 320, 180, 65'536u, false,
                automaticImage, automaticStats, error) ||
        !renderGlassPreview(renderer, automatic, 288, 288,
                            automaticGlassPreview, automaticGlassStats, error)) {
        fail("Auto-family GPU render failed: " + error);
    } else if (automaticStats.resolvedFamily !=
                   glasslight::resolveShapeFamily(automatic) ||
               automaticGlassStats.resolvedFamily !=
                   glasslight::resolveShapeFamily(automatic)) {
        fail("wall, glass preview, and core resolved Auto to different shape families");
    }

    for (const glasslight::ShapeFamily family : kFamilies) {
        glasslight::CompositionSettings settings;
        settings.seed = 0x9200000000000000ULL +
                        static_cast<std::uint32_t>(family) * 0x10101u;
        settings.shapeFamily = family;
        settings.quality = glasslight::QualityPreset::Draft;
        settings.phase = 0.3125f;

        glasslight::RenderedImage liveWall;
        glasslight::RenderedImage wallExport;
        glasslight::RenderedImage firstGlassPreview;
        glasslight::RenderedImage repeatedGlassPreview;
        glasslight::RenderStats liveWallStats;
        glasslight::RenderStats exportStats;
        glasslight::GlassPreviewStats firstGlassStats;
        glasslight::GlassPreviewStats repeatedGlassStats;
        if (!render(renderer, settings, 320, 180, 65'536u, false,
                    liveWall, liveWallStats, error) ||
            !render(renderer, settings, 320, 180, 65'536u, true,
                    wallExport, exportStats, error) ||
            !renderGlassPreview(renderer, settings, 288, 288,
                                firstGlassPreview, firstGlassStats, error) ||
            !renderGlassPreview(renderer, settings, 288, 288,
                                repeatedGlassPreview, repeatedGlassStats, error)) {
            fail(familySlug(family) + " render failed: " + error);
            continue;
        }
        if (liveWall.width != 320u || liveWall.height != 180u ||
            firstGlassPreview.width != 288u || firstGlassPreview.height != 288u ||
            repeatedGlassPreview.width != 288u || repeatedGlassPreview.height != 288u) {
            fail(familySlug(family) + " GPU preview ignored its requested dimensions");
        }
        if (liveWallStats.resolvedFamily != family || exportStats.resolvedFamily != family ||
            firstGlassStats.resolvedFamily != family ||
            repeatedGlassStats.resolvedFamily != family) {
            fail(familySlug(family) + " resolved to the wrong explicit family");
        }

        const ImageStructure liveWallStructure = analyze(liveWall);
        const ImageStructure exportStructure = analyze(wallExport);
        const ImageStructure firstGlassStructure = analyze(firstGlassPreview);
        const ImageStructure repeatedGlassStructure = analyze(repeatedGlassPreview);
        std::string reason;
        if (!structurallyUseful(liveWall, liveWallStats, liveWallStructure, reason)) {
            fail(familySlug(family) + " live wall is structurally empty: " + reason);
        }
        reason.clear();
        if (!structurallyUseful(wallExport, exportStats, exportStructure, reason)) {
            fail(familySlug(family) + " wall export is structurally empty: " + reason);
        }
        reason.clear();
        if (!previewStructurallyUseful(
                firstGlassPreview, firstGlassStats, firstGlassStructure, reason)) {
            fail(familySlug(family) + " glass preview is structurally empty: " + reason);
        }
        // Exact hashes are required only for two repeats on the same selected
        // GPU in this process. They are never used as cross-driver goldens.
        if (firstGlassStructure.hash != repeatedGlassStructure.hash) {
            fail(familySlug(family) + " repeated GPU preview was not deterministic");
        }
        reason.clear();
        if (!structurallySimilar(firstGlassStructure, repeatedGlassStructure, reason)) {
            fail(familySlug(family) + " repeated glass-preview structure changed: " + reason);
        }
        reason.clear();
        if (!structurallySimilar(liveWallStructure, exportStructure, reason)) {
            fail(familySlug(family) + " live-wall/export structure diverged: " + reason);
        }

        const std::filesystem::path previewPath =
            outputDirectory / (familySlug(family) + "-glass-preview.png");
        const std::filesystem::path wallPath =
            outputDirectory / (familySlug(family) + "-wall.png");
        if (!saveResult(previewPath, settings, firstGlassPreview, error) ||
            !saveResult(wallPath, settings, wallExport, error)) {
            fail(familySlug(family) + " PNG save/load artifact failed: " + error);
        }
        const glasslight::RenderStats previewReportStats =
            asReportStats(firstGlassStats, firstGlassPreview);
        writeReportLine(report, previewPath.filename().string(), settings, "glass-preview",
                        firstGlassPreview, previewReportStats, firstGlassStructure);
        writeReportLine(report, wallPath.filename().string(), settings, "wall",
                        wallExport, exportStats, exportStructure);
    }

    report.flush();
    if (!report) {
        fail("could not finish the structural metrics report");
    }
    if (failures == 0) {
        std::cout << "All six GPU families passed structural validation on "
                  << renderer.gpuName() << ".\n";
    }
    return failures == 0 ? 0 : 1;
}

int renderGallery(glasslight::VulkanRenderer& renderer,
                  const std::filesystem::path& outputDirectory) {
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create seed-gallery directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }
    std::ofstream report(outputDirectory / "manifest.tsv", std::ios::trunc);
    if (!report) {
        std::cerr << "Could not create seed-gallery manifest.\n";
        return 2;
    }
    writeReportHeader(report);

    constexpr std::array walls{
        glasslight::WallPreset::WarmWhitePaint,
        glasslight::WallPreset::CoolPlaster,
        glasslight::WallPreset::Charcoal};
    int failures = 0;
    std::string error;
    for (const glasslight::ShapeFamily family : kFamilies) {
        const std::filesystem::path familyDirectory = outputDirectory / familySlug(family);
        std::filesystem::create_directories(familyDirectory, filesystemError);
        if (filesystemError) {
            std::cerr << "FAIL: Could not create " << familyDirectory << ": "
                      << filesystemError.message() << '\n';
            ++failures;
            continue;
        }
        for (std::size_t index = 0; index < kGallerySeeds.size(); ++index) {
            glasslight::CompositionSettings settings;
            settings.seed = kGallerySeeds[index];
            settings.shapeFamily = family;
            settings.wall = walls[index];
            settings.quality = glasslight::QualityPreset::Draft;
            settings.phase = 0.125f + static_cast<float>(index) * 0.25f;

            for (const bool glassPreview : {true, false}) {
                const std::string mode = glassPreview ? "glass-preview" : "wall";
                const std::uint32_t width = glassPreview ? 480u : 640u;
                const std::uint32_t height = glassPreview ? 480u : 360u;
                glasslight::RenderedImage image;
                glasslight::RenderStats stats;
                glasslight::GlassPreviewStats previewStats;
                const bool rendered = glassPreview
                    ? renderGlassPreview(renderer, settings, width, height,
                                         image, previewStats, error)
                    : render(renderer, settings, width, height, 262'144u, true,
                             image, stats, error);
                if (!rendered) {
                    std::cerr << "FAIL: " << familySlug(family) << " seed "
                              << settings.seed << ' ' << mode << ": " << error << '\n';
                    ++failures;
                    continue;
                }
                if (glassPreview) {
                    stats = asReportStats(previewStats, image);
                }
                const ImageStructure structure = analyze(image);
                std::string reason;
                const bool useful = glassPreview
                    ? previewStructurallyUseful(image, previewStats, structure, reason)
                    : structurallyUseful(image, stats, structure, reason);
                if (!useful) {
                    std::cerr << "FAIL: " << familySlug(family) << " seed "
                              << settings.seed << ' ' << mode << ": " << reason << '\n';
                    ++failures;
                }
                const std::string filename = std::to_string(settings.seed) + "-" +
                    wallSlug(settings.wall) + "-" + mode + ".png";
                const std::filesystem::path outputPath = familyDirectory / filename;
                if (!saveResult(outputPath, settings, image, error)) {
                    std::cerr << "FAIL: Could not save " << outputPath << ": "
                              << error << '\n';
                    ++failures;
                    continue;
                }
                writeReportLine(report,
                                (std::filesystem::path(familySlug(family)) / filename).string(),
                                settings, mode, image, stats, structure);
            }
        }
    }
    report.flush();
    if (failures == 0) {
        std::cout << "Wrote reproducible 36-image seed gallery to "
                  << outputDirectory << " using " << renderer.gpuName() << ".\n";
    }
    return failures == 0 ? 0 : 1;
}

int benchmarkFamilies(glasslight::VulkanRenderer& renderer,
                      const std::filesystem::path& outputDirectory) {
    constexpr std::uint32_t width = 1280u;
    constexpr std::uint32_t height = 720u;
    constexpr std::uint32_t samples = 1'843'200u;
    constexpr double familyMaximumMilliseconds = 50.0;
    constexpr double fractureMaximumMilliseconds = 100.0;

    const double megapixels = static_cast<double>(width) * height / 1'000'000.0;
    const auto configuredSamples = static_cast<std::uint32_t>(std::llround(
        glasslight::photonsPerMegapixel(glasslight::QualityPreset::Standard) *
        megapixels));
    if (configuredSamples != samples) {
        std::cerr << "Standard photon budget changed: expected " << samples
                  << ", calculated " << configuredSamples << ".\n";
        return 2;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create benchmark directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }
    std::ofstream report(outputDirectory / "benchmark.tsv", std::ios::trunc);
    if (!report) {
        std::cerr << "Could not create benchmark report.\n";
        return 2;
    }
    report << "family\tmorphology\tmotion\tshards\tseed\twidth\theight\tsamples"
              "\tgpu_ms\tend_to_end_ms\tdeposits\thit_rate\tlimit_ms\tpassed\tgpu\n";

    // This full-size warm-up creates and caches the wall pipelines and grows
    // persistent output/accumulation buffers before any accepted frame is
    // timed.
    glasslight::CompositionSettings warmSettings;
    warmSettings.seed = 0x5741524d50495045ULL;
    warmSettings.shapeFamily = glasslight::ShapeFamily::Pebble;
    warmSettings.quality = glasslight::QualityPreset::Standard;
    warmSettings.phase = 0.375f;
    glasslight::RenderedImage warmImage;
    glasslight::RenderStats warmStats;
    std::string error;
    if (!render(renderer, warmSettings, width, height, samples, false,
                warmImage, warmStats, error)) {
        std::cerr << "Could not warm persistent render pipelines: " << error << '\n';
        return 2;
    }

    // Match the Studio's steady-state path: its RenderedImage survives between
    // frames, so the host pixel vector retains capacity after warm-up.
    glasslight::RenderedImage image = std::move(warmImage);
    int failures = 0;
    const auto benchmark = [&](const glasslight::CompositionSettings& settings,
                               const std::string& label, const double limit,
                               const bool enforceTiming) {
        glasslight::RenderStats caseWarmStats;
        if (!render(renderer, settings, width, height, samples, false,
                    image, caseWarmStats, error)) {
            std::cerr << "FAIL: " << label << " benchmark warm-up failed: " << error << '\n';
            ++failures;
            return;
        }
        glasslight::RenderStats stats;
        if (!render(renderer, settings, width, height, samples, false,
                    image, stats, error)) {
            std::cerr << "FAIL: " << label << " benchmark render failed: " << error << '\n';
            ++failures;
            return;
        }
        const auto family = settings.shapeFamily;
        const bool requestIsExact = stats.requestedSamples == samples &&
                                    stats.tracedSamples == samples &&
                                    image.width == width && image.height == height &&
                                    stats.resolvedFamily == family;
        const bool timingIsValid = std::isfinite(stats.endToEndMilliseconds) &&
                                   stats.endToEndMilliseconds > 0.0;
        const bool passed = requestIsExact && timingIsValid &&
                            (!enforceTiming || stats.endToEndMilliseconds <= limit);
        report << familySlug(family) << '\t'
               << (family == glasslight::ShapeFamily::Fracture
                       ? morphologySlug(settings.fractureMorphology) : "-") << '\t'
               << (family == glasslight::ShapeFamily::Fracture
                       ? motionSlug(settings.fractureMotion) : "-") << '\t'
               << (family == glasslight::ShapeFamily::Fracture
                       ? std::to_string(settings.fractureShardCount) : "-") << '\t'
               << settings.seed << '\t' << width << '\t' << height << '\t' << samples << '\t'
               << std::fixed << std::setprecision(3) << image.renderMilliseconds << '\t'
               << stats.endToEndMilliseconds << '\t' << stats.depositedSamples << '\t'
               << std::setprecision(6) << stats.estimatedHitRate << '\t'
               << std::setprecision(3) << (enforceTiming ? std::to_string(limit) : "measured")
               << '\t' << (passed ? "yes" : "no") << '\t' << tsvSafe(image.gpuName) << '\n';
        if (!passed) {
            std::cerr << "FAIL: " << label << " cached 1280x720 frame took "
                      << stats.endToEndMilliseconds << " ms";
            if (enforceTiming) std::cerr << " (limit " << limit << " ms)";
            if (!requestIsExact) {
                std::cerr << "; request dimensions, samples, or resolved family changed";
            }
            std::cerr << ".\n";
            ++failures;
        }
    };

    for (const glasslight::ShapeFamily family : kFamilies) {
        if (family == glasslight::ShapeFamily::Fracture) continue;
        glasslight::CompositionSettings settings;
        settings.seed = 0xb200000000000000ULL +
                        static_cast<std::uint32_t>(family) * 0x10001u;
        settings.shapeFamily = family;
        settings.quality = glasslight::QualityPreset::Standard;
        settings.phase = 0.375f;

        benchmark(settings, familySlug(family), familyMaximumMilliseconds, true);
    }

    // Load and warm the specialized analytic pipeline before measuring any
    // accepted Fracture frame. Legacy families above never pay for or retain
    // this pipeline during their own 50 ms gate.
    glasslight::CompositionSettings fractureWarm;
    fractureWarm.seed = 0x465241435741524dULL;
    fractureWarm.shapeFamily = glasslight::ShapeFamily::Fracture;
    fractureWarm.quality = glasslight::QualityPreset::Standard;
    fractureWarm.fractureShardCount = 16;
    fractureWarm.fractureMorphology = glasslight::FractureMorphology::Hybrid;
    fractureWarm.fractureMotion = glasslight::FractureMotion::BreakAndReform;
    fractureWarm.phase = 0.375f;
    glasslight::RenderStats fractureWarmStats;
    if (!render(renderer, fractureWarm, width, height, samples, false,
                image, fractureWarmStats, error)) {
        std::cerr << "Could not warm Fracture render pipeline: " << error << '\n';
        return 2;
    }

    constexpr std::array morphologies{
        glasslight::FractureMorphology::Growth,
        glasslight::FractureMorphology::Exploded,
        glasslight::FractureMorphology::Hybrid};
    constexpr std::array motions{
        glasslight::FractureMotion::Rigid,
        glasslight::FractureMotion::GrowAndRecede,
        glasslight::FractureMotion::BreakAndReform};
    for (const std::uint32_t count : {9u, 16u}) {
        for (const auto morphology : morphologies) {
            for (const auto motion : motions) {
                glasslight::CompositionSettings settings;
                settings.seed = 0xb900000000000000ULL +
                    static_cast<std::uint64_t>(morphology) * 0x10001u +
                    static_cast<std::uint64_t>(motion) * 0x101u + count;
                settings.shapeFamily = glasslight::ShapeFamily::Fracture;
                settings.quality = glasslight::QualityPreset::Standard;
                settings.phase = 0.375f;
                settings.fractureShardCount = count;
                settings.fractureMorphology = morphology;
                settings.fractureMotion = motion;
                const std::string label = "fracture/" + morphologySlug(morphology) + "/" +
                    motionSlug(motion) + "/" + std::to_string(count);
                benchmark(settings, label, fractureMaximumMilliseconds, count == 9u);
            }
        }
    }
    report.flush();
    if (!report) {
        std::cerr << "FAIL: Could not finish benchmark report.\n";
        ++failures;
    }
    if (failures == 0) {
        std::cout << "All five SDF families met 50 ms and all forced nine-shard "
                     "Fracture variants met 100 ms on " << renderer.gpuName()
                  << "; sixteen-shard variants were measured without a hard gate.\n";
    } else {
        std::cerr << "Benchmark report: " << outputDirectory / "benchmark.tsv" << '\n';
    }
    return failures == 0 ? 0 : 1;
}

void printUsage() {
    std::cout
        << "GlassLight GPU render validation\n"
        << "  glasslight-render-validation --validate OUTPUT_DIRECTORY\n"
        << "  glasslight-render-validation --gallery OUTPUT_DIRECTORY\n"
        << "  glasslight-render-validation --fracture OUTPUT_DIRECTORY\n"
        << "  glasslight-render-validation --benchmark OUTPUT_DIRECTORY\n"
        << "Use GLASSLIGHT_GPU to select the same device as the application.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::string(argv[1]) != "--validate" &&
                      std::string(argv[1]) != "--gallery" &&
                      std::string(argv[1]) != "--fracture" &&
                      std::string(argv[1]) != "--benchmark")) {
        printUsage();
        return argc == 2 && (std::string(argv[1]) == "--help" ||
                             std::string(argv[1]) == "-h") ? 0 : 2;
    }

    glasslight::VulkanRenderer renderer;
    std::string error;
    const char* preferredGpu = std::getenv("GLASSLIGHT_GPU");
    if (!renderer.initialize(preferredGpu ? preferredGpu : "", error)) {
        std::cerr << "GPU validation unavailable: " << error << '\n';
        return 2;
    }
    const std::filesystem::path outputDirectory(argv[2]);
    if (std::string(argv[1]) == "--gallery") {
        return renderGallery(renderer, outputDirectory);
    }
    if (std::string(argv[1]) == "--fracture") {
        return validateFracture(renderer, outputDirectory);
    }
    if (std::string(argv[1]) == "--benchmark") {
        return benchmarkFamilies(renderer, outputDirectory);
    }
    return validateFamilies(renderer, outputDirectory);
}
