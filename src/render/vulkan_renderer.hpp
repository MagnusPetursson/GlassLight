#pragma once

#include "core/composition.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace glasslight {

struct FirstLightRequest {
    std::uint32_t width = 256;
    std::uint32_t height = 144;
    std::uint32_t samples = 1u << 20u;
    std::uint32_t seed = 0x474c4153u;
    float ior = 1.52f;
    float roughness = 0.06f;
    float lightX = -0.35f;
    float lightY = 0.25f;
    float intensity = 3.0f;
    float causticStrength = 1.5f;
    float phase = 0.0f;
};

struct RenderedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> rgba8;
    std::string gpuName;
    double renderMilliseconds = 0.0;
    std::uint64_t nonBackgroundPixels = 0;
};

// One deterministic compute render. sampleOffset selects a disjoint portion of
// the seed's low-discrepancy sequence, which lets callers schedule preview
// passes or export-sized batches without changing the resulting composition.
struct RenderRequest {
    CompositionSettings settings{};
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t samples = 1u << 20u;
    std::uint64_t sampleOffset = 0;
    std::uint32_t progressivePass = 0;
    bool exportRender = false;
};

struct RenderStats {
    std::uint64_t requestedSamples = 0;
    std::uint64_t tracedSamples = 0;
    std::uint64_t depositedSamples = 0;
    std::uint32_t dispatchCount = 0;
    double estimatedHitRate = 0.0;
    ShapeFamily resolvedFamily = ShapeFamily::Pebble;
    std::uint64_t sampleOffset = 0;
    std::uint64_t interfaceEvents = 0;
    std::uint64_t internalReflections = 0;
    std::uint64_t traversalExhausted = 0;
    double endToEndMilliseconds = 0.0;
};

struct GlassPreviewRequest {
    CompositionSettings settings{};
    std::uint32_t width = 640;
    std::uint32_t height = 640;
    float orbitYaw = 0.55f;
    float orbitPitch = 0.25f;
    float zoom = 1.0f;
    bool edgeOverlay = true;
};

struct GlassPreviewStats {
    ShapeFamily resolvedFamily = ShapeFamily::Pebble;
    std::uint64_t shadedPixels = 0;
    double endToEndMilliseconds = 0.0;
};

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool initialize(std::string& error);
    bool initialize(const std::string& preferredGpuName, std::string& error);
    static std::vector<std::string> compatibleGpuNames(std::string& error);
    bool render(const RenderRequest& request, RenderedImage& output,
                RenderStats& stats, std::string& error);
    bool render(const RenderRequest& request, RenderedImage& output,
                std::string& error);
    bool renderGlassPreview(const GlassPreviewRequest& request, RenderedImage& output,
                            GlassPreviewStats& stats, std::string& error);
    bool renderGlassPreview(const GlassPreviewRequest& request, RenderedImage& output,
                            std::string& error);
    bool renderFirstLight(const FirstLightRequest& request, RenderedImage& output,
                          std::string& error);
    [[nodiscard]] const std::string& gpuName() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

bool savePng(const RenderedImage& image, const std::filesystem::path& path,
             std::string& error);

} // namespace glasslight
