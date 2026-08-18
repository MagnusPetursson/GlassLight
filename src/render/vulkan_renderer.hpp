#pragma once

#include "core/composition.hpp"
#include "render/adaptive_controller.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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
    // For progressive refinement, samples is this pass and resolveSamples is
    // the accumulated total used for exposure. Zero means samples.
    std::uint32_t resolveSamples = 0;
    std::uint64_t sampleOffset = 0;
    std::uint32_t progressivePass = 0;
    bool preserveAccumulation = false;
    bool exportRender = false;
    // Interactive callers may temporarily lower traversal depth without
    // changing or serializing the selected Quality ceiling.
    std::optional<QualityPreset> interactiveInterfaceDepth;
    std::optional<std::uint32_t> slotAffinity;
    // Release-benchmark candidates only. Runtime callers use the selected,
    // capability-neutral 256/16 defaults on every device.
    std::uint32_t traceGroupSize = 256;
    std::uint32_t imageGroupSize = 16;
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

struct RenderTicket {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    [[nodiscard]] explicit operator bool() const { return generation != 0; }
    friend bool operator==(const RenderTicket&, const RenderTicket&) = default;
};

enum class RenderPollState : std::uint8_t {
    Pending,
    Ready,
    Superseded,
    Failed
};

struct ReadbackFrameView {
    const std::uint32_t* rgba8 = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitchBytes = 0;

    [[nodiscard]] explicit operator bool() const { return rgba8 != nullptr; }
};

struct RenderPollResult {
    RenderPollState state = RenderPollState::Pending;
    ReadbackFrameView frame{};
    RenderTimings timings{};
    RenderStats stats{};
    std::string error;
};

struct MemoryAllocationDiagnostics {
    std::uint32_t memoryTypeIndex = 0;
    bool deviceLocal = false;
    bool hostVisible = false;
    bool hostCached = false;
    bool hostCoherent = false;
};

struct RendererDiagnostics {
    std::string deviceName;
    std::string driverName;
    std::uint32_t driverVersion = 0;
    bool timestampAvailable = false;
    float timestampPeriodNanoseconds = 0.0f;
    MemoryAllocationDiagnostics accumulation{};
    MemoryAllocationDiagnostics settings{};
    MemoryAllocationDiagnostics upload{};
    MemoryAllocationDiagnostics readback{};
    std::uint64_t transientAllocationBytes = 0;
    std::uint32_t settingsPayloadBytes = 0;
};

struct GlassPreviewRequest {
    CompositionSettings settings{};
    std::uint32_t width = 640;
    std::uint32_t height = 640;
    float orbitYaw = 0.55f;
    float orbitPitch = 0.25f;
    float zoom = 1.0f;
    bool edgeOverlay = true;
    bool interactive = false;
    std::uint32_t imageGroupSize = 16;
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
    RenderTicket submit(const RenderRequest& request, std::string& error);
    RenderTicket submitGlassPreview(const GlassPreviewRequest& request,
                                    std::string& error);
    RenderPollResult poll(RenderTicket ticket);
    void release(RenderTicket ticket);
    void recordSdlUpload(RenderTicket ticket, double milliseconds);
    void trimTransientResources();
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
    [[nodiscard]] RendererDiagnostics diagnostics() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

bool savePng(const RenderedImage& image, const std::filesystem::path& path,
             std::string& error);

} // namespace glasslight
