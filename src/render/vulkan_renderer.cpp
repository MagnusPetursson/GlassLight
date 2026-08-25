#include "render/vulkan_renderer.hpp"
#include "platform/platform.hpp"
#include "core/fracture_layout.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#ifndef GLASSLIGHT_SHADER_DIR
#define GLASSLIGHT_SHADER_DIR "shaders"
#endif
#ifndef GLASSLIGHT_VERSION_MAJOR
#define GLASSLIGHT_VERSION_MAJOR 0
#define GLASSLIGHT_VERSION_MINOR 2
#define GLASSLIGHT_VERSION_PATCH 0
#endif

namespace glasslight {
namespace {

std::string vkError(const char* operation, VkResult result) {
    return std::string(operation) + " failed with VkResult " + std::to_string(result) + ".";
}

VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    if (callbackData && callbackData->pMessage) {
        std::cerr << "Vulkan validation: " << callbackData->pMessage << '\n';
    }
    return VK_FALSE;
}

std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open shader: " + platform::pathToUtf8(path);
        return {};
    }
    const std::streamsize bytes = input.tellg();
    if (bytes <= 0 || bytes % 4 != 0) {
        error = "Shader is empty or not valid SPIR-V: " + platform::pathToUtf8(path);
        return {};
    }
    input.seekg(0);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4u);
    if (!input.read(reinterpret_cast<char*>(words.data()), bytes)) {
        error = "Could not read shader: " + platform::pathToUtf8(path);
        return {};
    }
    return words;
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    std::uint32_t memoryTypeIndex = 0;
    VkMemoryPropertyFlags memoryFlags = 0;
};

struct TracePush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t dispatchSamples;
    std::uint32_t sampleBaseLow;
    std::uint32_t sampleBaseHigh;
    std::uint32_t totalSamples;
    std::uint32_t progressivePass;
    std::uint32_t exportRender;
};

struct ResolvePush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t sampleCount;
    std::uint32_t padding;
    float exposure;
    float wallR;
    float wallG;
    float wallB;
    float wallGrain;
    float projectionStrength;
    std::uint32_t seed;
    std::uint32_t wallPreset;
};

struct PreviewPush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t edgeOverlay;
    std::uint32_t interactive;
    float orbitYaw;
    float orbitPitch;
    float zoom;
    float padding2;
};

struct alignas(16) GpuSettingsCommon {
    // vec4-aligned layout shared with the shader's std430 storage block.
    std::array<std::array<float, 4>, 5> palette{};
    std::array<std::uint32_t, 4> shapeRegion{};
    std::array<float, 4> geometry{};
    std::array<float, 4> optical{};
    std::array<float, 4> light{};
    std::array<float, 4> rotation{};
    std::array<std::uint32_t, 4> renderOptions{};
};

struct alignas(16) LegacyGpuSettings {
    GpuSettingsCommon common{};
    // Family-specialized constants and deterministic region descriptors. These
    // replace hashing and seed decoding in the repeated SDF/tint hot paths.
    std::array<std::array<float, 4>, 4> legacyShape{};
    std::array<std::array<float, 4>, 24> legacyCells{};
};

struct alignas(16) FractureGpuSettings {
    GpuSettingsCommon common{};
    std::array<std::uint32_t, 4> fractureInfo{};
    std::array<float, 4> fractureBounds{};
    std::array<std::array<float, 4>, 4> fractureGroupBounds{};
    struct Shard {
        std::array<float, 4> baseLength{};
        std::array<float, 4> axisRadius{};
        std::array<float, 4> basisTip{};
        std::array<float, 4> separationProfile{};
        std::array<std::uint32_t, 4> paletteSidesPlanes{};
    };
    std::array<Shard, kMaximumFractureShards> fractureShards{};
    std::array<std::array<std::array<float, 4>, kMaximumFracturePlanes>,
               kMaximumFractureShards> fracturePlanes{};
};

static_assert(sizeof(TracePush) == 32);
static_assert(sizeof(ResolvePush) == 48);
static_assert(sizeof(PreviewPush) == 32);
static_assert(sizeof(GpuSettingsCommon) == 176);
static_assert(alignof(GpuSettingsCommon) == 16);
static_assert(std::is_standard_layout_v<GpuSettingsCommon>);
static_assert(offsetof(GpuSettingsCommon, palette) == 0);
static_assert(offsetof(GpuSettingsCommon, shapeRegion) == 80);
static_assert(offsetof(GpuSettingsCommon, geometry) == 96);
static_assert(offsetof(GpuSettingsCommon, optical) == 112);
static_assert(offsetof(GpuSettingsCommon, light) == 128);
static_assert(offsetof(GpuSettingsCommon, rotation) == 144);
static_assert(offsetof(GpuSettingsCommon, renderOptions) == 160);
static_assert(sizeof(LegacyGpuSettings) == 624);
static_assert(alignof(LegacyGpuSettings) == 16);
static_assert(std::is_standard_layout_v<LegacyGpuSettings>);
static_assert(offsetof(LegacyGpuSettings, common) == 0);
static_assert(offsetof(LegacyGpuSettings, legacyShape) == 176);
static_assert(offsetof(LegacyGpuSettings, legacyCells) == 240);
static_assert(sizeof(FractureGpuSettings::Shard) == 80);
static_assert(sizeof(FractureGpuSettings) == 4880);
static_assert(alignof(FractureGpuSettings) == 16);
static_assert(std::is_standard_layout_v<FractureGpuSettings>);
static_assert(offsetof(FractureGpuSettings, common) == 0);
static_assert(offsetof(FractureGpuSettings, fractureInfo) == 176);
static_assert(offsetof(FractureGpuSettings, fractureBounds) == 192);
static_assert(offsetof(FractureGpuSettings, fractureGroupBounds) == 208);
static_assert(offsetof(FractureGpuSettings, fractureShards) == 272);
static_assert(offsetof(FractureGpuSettings, fracturePlanes) == 1552);

struct PreparedGpuSettings {
    alignas(16) std::array<std::byte, sizeof(FractureGpuSettings)> bytes{};
    std::size_t size = 0;
};

std::array<float, 3> wallColor(WallPreset wall) {
    switch (wall) {
    case WallPreset::NeutralGallery: return {0.90f, 0.90f, 0.88f};
    case WallPreset::CoolPlaster: return {0.78f, 0.84f, 0.88f};
    case WallPreset::Charcoal: return {0.10f, 0.115f, 0.13f};
    case WallPreset::WarmWhitePaint:
    default: return {0.93f, 0.90f, 0.84f};
    }
}

std::uint32_t settingsHash32(std::uint32_t value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float settingsRandom01(const std::uint32_t value) {
    return static_cast<float>(settingsHash32(value)) * (1.0f / 4294967296.0f);
}

template <typename Settings>
void populateCommonGpuSettings(Settings& gpu, const CompositionSettings& settings,
                               const ShapeFamily family, const std::uint32_t seed,
                               const std::optional<QualityPreset> interfaceDepth) {
    for (std::size_t index = 0; index < settings.palette.colors.size(); ++index) {
        const Color3 color = settings.palette.colors[index];
        gpu.palette[index] = {color.r, color.g, color.b, 0.0f};
    }
    gpu.shapeRegion = {
        static_cast<std::uint32_t>(family), settings.regionCount,
        settings.includeClearRegions ? 1u : 0u, seed
    };
    gpu.geometry = {
        settings.shapeScale, settings.regionSoftness,
        settings.colorDepth, settings.dispersion
    };
    gpu.optical = {
        settings.ior, settings.roughness,
        settings.lightDistance, settings.lightRadius
    };
    gpu.light = {
        settings.lightX, settings.lightY,
        settings.lightIntensity, settings.projectionStrength
    };
    float yaw = settings.axisYaw;
    float pitch = settings.axisPitch;
    if (!settings.overrideAxis) {
        yaw = (static_cast<float>((seed >> 8u) & 0xffffu) / 65535.0f - 0.5f) * 2.2f;
        pitch = 0.35f + static_cast<float>((seed >> 24u) & 0xffu) / 255.0f * 0.75f;
    }
    const float cp = std::cos(pitch);
    const std::array axis{
        cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch)};
    const float halfAngle = settings.phase * 3.14159265358979323846f;
    const float sine = std::sin(halfAngle);
    gpu.rotation = {axis[0] * sine, axis[1] * sine,
                    axis[2] * sine, std::cos(halfAngle)};
    const float fresnel0 = std::pow((settings.ior - 1.0f) /
                                    (settings.ior + 1.0f), 2.0f);
    gpu.renderOptions = {static_cast<std::uint32_t>(
        interfaceDepth.value_or(settings.quality)), 0u,
        std::bit_cast<std::uint32_t>(fresnel0), 0u};
}

PreparedGpuSettings makeGpuSettings(const CompositionSettings& settings,
                                    ShapeFamily family, std::uint32_t seed,
                                    const std::optional<QualityPreset> interfaceDepth = {}) {
    PreparedGpuSettings prepared;
    if (family == ShapeFamily::Fracture) {
        FractureGpuSettings gpu{};
        populateCommonGpuSettings(gpu.common, settings, family, seed, interfaceDepth);
        const FractureLayout layout = buildFractureLayout(settings);
        gpu.fractureInfo = {
            layout.shardCount,
            static_cast<std::uint32_t>(layout.morphology),
            static_cast<std::uint32_t>(layout.motion),
            0u
        };
        gpu.fractureBounds = {layout.boundsCenter[0], layout.boundsCenter[1],
                              layout.boundsCenter[2], layout.boundsRadius};
        for (std::uint32_t group = 0; group < 4u; ++group) {
            const std::uint32_t begin = group * 4u;
            const std::uint32_t end = std::min(begin + 4u, layout.shardCount);
            if (begin >= end) continue;
            std::array minimum{1.0e10f, 1.0e10f, 1.0e10f};
            std::array maximum{-1.0e10f, -1.0e10f, -1.0e10f};
            for (std::uint32_t index = begin; index < end; ++index) {
                const FractureShard& shard = layout.shards[index];
                const std::array center{
                    shard.base[0] + shard.axis[0] * shard.length * 0.5f,
                    shard.base[1] + shard.axis[1] * shard.length * 0.5f,
                    shard.base[2] + shard.axis[2] * shard.length * 0.5f};
                const float radiusFactor = shard.polygonSides == 3u ? 2.0f :
                    (shard.polygonSides == 4u ? 1.414214f :
                     (shard.polygonSides == 5u ? 1.236068f : 1.154701f));
                const float circumradius = shard.radius * radiusFactor;
                const float radius = std::sqrt(shard.length * shard.length * 0.25f +
                                               circumradius * circumradius);
                for (std::size_t axisIndex = 0; axisIndex < 3u; ++axisIndex) {
                    minimum[axisIndex] = std::min(minimum[axisIndex],
                                                  center[axisIndex] - radius);
                    maximum[axisIndex] = std::max(maximum[axisIndex],
                                                  center[axisIndex] + radius);
                }
            }
            const std::array center{
                (minimum[0] + maximum[0]) * 0.5f,
                (minimum[1] + maximum[1]) * 0.5f,
                (minimum[2] + maximum[2]) * 0.5f};
            float radius = 0.0f;
            for (std::uint32_t index = begin; index < end; ++index) {
                const FractureShard& shard = layout.shards[index];
                const std::array shardCenter{
                    shard.base[0] + shard.axis[0] * shard.length * 0.5f,
                    shard.base[1] + shard.axis[1] * shard.length * 0.5f,
                    shard.base[2] + shard.axis[2] * shard.length * 0.5f};
                const float radiusFactor = shard.polygonSides == 3u ? 2.0f :
                    (shard.polygonSides == 4u ? 1.414214f :
                     (shard.polygonSides == 5u ? 1.236068f : 1.154701f));
                const float circumradius = shard.radius * radiusFactor;
                const float shardRadius = std::sqrt(
                    shard.length * shard.length * 0.25f + circumradius * circumradius);
                const float dx = shardCenter[0] - center[0];
                const float dy = shardCenter[1] - center[1];
                const float dz = shardCenter[2] - center[2];
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) +
                                          shardRadius);
            }
            gpu.fractureGroupBounds[group] = {center[0], center[1], center[2], radius};
        }
        for (std::uint32_t index = 0; index < layout.shardCount; ++index) {
            const FractureShard& source = layout.shards[index];
            auto& target = gpu.fractureShards[index];
            target.baseLength = {source.base[0], source.base[1], source.base[2],
                                 source.length};
            target.axisRadius = {source.axis[0], source.axis[1], source.axis[2],
                                 source.radius};
            target.basisTip = {source.basisU[0], source.basisU[1], source.basisU[2],
                               source.tipFraction};
            target.separationProfile = {
                source.separationDirection[0], source.separationDirection[1],
                source.separationDirection[2], static_cast<float>(source.profile)
            };
            target.paletteSidesPlanes = {
                source.primaryPalette, source.secondaryPalette,
                source.polygonSides, source.planeCount
            };
            for (std::uint32_t plane = 0; plane < source.planeCount; ++plane) {
                const FracturePlane& value = source.planes[plane];
                gpu.fracturePlanes[index][plane] = {
                    value.normal[0], value.normal[1], value.normal[2], value.offset
                };
            }
        }
        std::memcpy(prepared.bytes.data(), &gpu, sizeof(gpu));
        prepared.size = sizeof(gpu);
        return prepared;
    }

    LegacyGpuSettings gpu{};
    populateCommonGpuSettings(gpu.common, settings, family, seed, interfaceDepth);
    switch (family) {
    case ShapeFamily::Lens: {
        const float ellipticity = 0.88f + 0.15f * settingsRandom01(seed ^ 0x718u);
        gpu.legacyShape[0] = {ellipticity, 1.0f / ellipticity, 0.0f, 0.0f};
        break;
    }
    case ShapeFamily::Ribbon:
        gpu.legacyShape[0] = {
            ((seed >> 5u) & 1u) == 0u ? 2.0f : 3.0f,
            ((seed >> 7u) & 1u) == 0u ? 1.0f : -1.0f, 0.0f, 0.0f};
        break;
    case ShapeFamily::FacetedVessel:
        gpu.legacyShape[0] = {
            static_cast<float>(8u + 2u * ((seed >> 9u) % 3u)),
            static_cast<float>(settingsHash32(seed ^ 0x243f6a88u) % 4u),
            0.96f + 0.08f * settingsRandom01(seed ^ 0x85ebca6bu),
            ((seed >> 15u) & 1u) == 0u ? 0.0f : 1.0f};
        gpu.legacyShape[1][0] =
            0.085f + 0.015f * settingsRandom01(seed ^ 0xc2b2ae35u);
        break;
    case ShapeFamily::CutCrystal:
        gpu.legacyShape[0] = {
            ((seed >> 7u) & 1u) == 0u ? 0.0f : 1.0f,
            ((seed >> 13u) & 1u) == 0u ? 0.0f : 1.0f,
            static_cast<float>((seed >> 19u) & 3u), 0.0f};
        break;
    case ShapeFamily::Pebble:
    case ShapeFamily::Auto:
    default: {
        const std::array stretch{
            0.88f + 0.12f * settingsRandom01(seed ^ 0x21u),
            1.02f + 0.15f * settingsRandom01(seed ^ 0x37u),
            0.83f + 0.16f * settingsRandom01(seed ^ 0x51u)};
        gpu.legacyShape[0] = {
            stretch[0], stretch[1], stretch[2],
            std::min(stretch[0], std::min(stretch[1], stretch[2]))};
        gpu.legacyShape[1][0] = static_cast<float>(seed & 255u) / 255.0f - 0.5f;
        break;
    }
    }
    for (std::uint32_t index = 0u; index < gpu.legacyCells.size(); ++index) {
        const std::uint32_t base = settingsHash32(seed ^ (index * 0x9e3779b9u));
        const bool clear = settings.includeClearRegions &&
            settingsHash32(seed + index * 19u) % 5u == 0u;
        const std::uint32_t tintIndex = clear ? 5u : index % 5u;
        gpu.legacyCells[index] = {
            settingsRandom01(base) * 1.7f - 0.85f,
            settingsRandom01(base ^ 0x68bc21ebu) * 1.7f - 0.85f,
            settingsRandom01(base ^ 0x02e5be93u) * 1.7f - 0.85f,
            std::bit_cast<float>(tintIndex)};
    }
    std::memcpy(prepared.bytes.data(), &gpu, sizeof(gpu));
    prepared.size = sizeof(gpu);
    return prepared;
}

} // namespace

struct VulkanRenderer::Impl {
    enum class SlotState { Free, Submitted, Ready, Failed };
    struct FrameSlot {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        Buffer accumulation;
        Buffer resolved;
        Buffer settings;
        Buffer statistics;
        Buffer upload;
        Buffer readback;
        VkDeviceSize settingsPayloadSize = sizeof(LegacyGpuSettings);
        SlotState state = SlotState::Free;
        bool releaseRequested = false;
        std::uint64_t generation = 0;
        bool glassPreview = false;
        RenderRequest request{};
        GlassPreviewRequest previewRequest{};
        RenderStats stats{};
        RenderTimings timings{};
        std::string failure;
        std::chrono::steady_clock::time_point submittedAt{};
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    std::uint32_t maxDispatchX = 65535;
    std::string selectedGpu;
    std::string driverName;
    std::uint32_t driverVersion = 0;
    std::uint32_t timestampValidBits = 0;
    float timestampPeriod = 0.0f;
    std::chrono::steady_clock::time_point lastArtworkCompletion{};
    std::chrono::steady_clock::time_point lastPreviewCompletion{};
    std::uint64_t latestGeneration = 0;
    std::uint64_t latestArtworkGeneration = 0;
    std::uint64_t latestPreviewGeneration = 0;
    mutable std::mutex mutex;

    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkShaderModule traceShader = VK_NULL_HANDLE;
    VkShaderModule fractureTraceShader = VK_NULL_HANDLE;
    VkShaderModule resolveShader = VK_NULL_HANDLE;
    VkShaderModule previewShader = VK_NULL_HANDLE;
    VkShaderModule fracturePreviewShader = VK_NULL_HANDLE;
    VkShaderModule fractureTrace128Shader = VK_NULL_HANDLE;
    VkShaderModule resolve8Shader = VK_NULL_HANDLE;
    VkShaderModule fracturePreview8Shader = VK_NULL_HANDLE;
    std::array<VkShaderModule, 5> familyTraceShaders{};
    std::array<VkShaderModule, 5> familyPreviewShaders{};
    std::array<VkShaderModule, 5> familyTrace128Shaders{};
    std::array<VkShaderModule, 5> familyPreview8Shaders{};
    VkPipeline tracePipeline = VK_NULL_HANDLE;
    VkPipeline fractureTracePipeline = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE;
    VkPipeline previewPipeline = VK_NULL_HANDLE;
    VkPipeline fracturePreviewPipeline = VK_NULL_HANDLE;
    VkPipeline fractureTrace128Pipeline = VK_NULL_HANDLE;
    VkPipeline resolve8Pipeline = VK_NULL_HANDLE;
    VkPipeline fracturePreview8Pipeline = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> familyTracePipelines{};
    std::array<VkPipeline, 5> familyPreviewPipelines{};
    std::array<VkPipeline, 5> familyTrace128Pipelines{};
    std::array<VkPipeline, 5> familyPreview8Pipelines{};
    std::array<FrameSlot, 2> slots{};
    RendererDiagnostics diagnosticSnapshot{};

    ~Impl() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        for (FrameSlot& slot : slots) {
            destroyBuffer(slot.readback);
            destroyBuffer(slot.upload);
            destroyBuffer(slot.statistics);
            destroyBuffer(slot.settings);
            destroyBuffer(slot.resolved);
            destroyBuffer(slot.accumulation);
            if (slot.queryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, slot.queryPool, nullptr);
            }
            if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(device, slot.fence, nullptr);
        }
        if (fracturePreviewPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fracturePreviewPipeline, nullptr);
        if (fracturePreview8Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fracturePreview8Pipeline, nullptr);
        for (VkPipeline pipeline : familyPreviewPipelines) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (previewPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, previewPipeline, nullptr);
        if (resolvePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, resolvePipeline, nullptr);
        if (resolve8Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, resolve8Pipeline, nullptr);
        if (fractureTracePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fractureTracePipeline, nullptr);
        if (fractureTrace128Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fractureTrace128Pipeline, nullptr);
        if (tracePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, tracePipeline, nullptr);
        for (VkPipeline pipeline : familyTracePipelines) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        }
        for (VkPipeline pipeline : familyPreview8Pipelines) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        }
        for (VkPipeline pipeline : familyTrace128Pipelines) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (fracturePreviewShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fracturePreviewShader, nullptr);
        if (fracturePreview8Shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fracturePreview8Shader, nullptr);
        if (previewShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, previewShader, nullptr);
        if (resolveShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, resolveShader, nullptr);
        if (resolve8Shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, resolve8Shader, nullptr);
        if (fractureTraceShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fractureTraceShader, nullptr);
        if (fractureTrace128Shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fractureTrace128Shader, nullptr);
        if (traceShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, traceShader, nullptr);
        for (VkShaderModule shader : familyPreviewShaders) {
            if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
        }
        for (VkShaderModule shader : familyTraceShaders) {
            if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
        }
        for (VkShaderModule shader : familyPreview8Shaders) {
            if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
        }
        for (VkShaderModule shader : familyTrace128Shaders) {
            if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
        }
        if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (allocator != VK_NULL_HANDLE) vmaDestroyAllocator(allocator);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (debugMessenger != VK_NULL_HANDLE) {
            const auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyDebug) destroyDebug(instance, debugMessenger, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    bool createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage,
                      const VmaMemoryUsage memoryUsage,
                      const VmaAllocationCreateFlags flags,
                      const VkMemoryPropertyFlags requiredFlags,
                      Buffer& result, std::string& error) const {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = memoryUsage;
        allocationInfo.flags = flags;
        allocationInfo.requiredFlags = requiredFlags;
        VmaAllocationInfo actual{};
        const VkResult status = vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo,
                                                &result.buffer, &result.allocation,
                                                &actual);
        if (status != VK_SUCCESS) {
            error = vkError("vmaCreateBuffer", status);
            return false;
        }
        result.size = size;
        result.mapped = actual.pMappedData;
        result.memoryTypeIndex = actual.memoryType;
        vmaGetAllocationMemoryProperties(allocator, result.allocation,
                                         &result.memoryFlags);
        return true;
    }

    void destroyBuffer(Buffer& buffer) const {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
        }
        buffer = {};
    }

    VkShaderModule createShader(const std::filesystem::path& path, std::string& error) const {
        std::vector<std::uint32_t> words = readSpirv(path, error);
        if (words.empty()) {
            const char* base = SDL_GetBasePath();
            if (base != nullptr) {
                const auto fileName = path.filename();
                const std::array<std::filesystem::path, 2> installedCandidates{{
                    platform::pathFromUtf8(base) / ".." / "share" / "glasslight" /
                        "shaders" / fileName,
                    platform::pathFromUtf8(base) / "shaders" / fileName
                }};
                for (const auto& candidate : installedCandidates) {
                    std::string candidateError;
                    words = readSpirv(candidate.lexically_normal(), candidateError);
                    if (!words.empty()) {
                        error.clear();
                        break;
                    }
                }
            }
        }
        if (words.empty()) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = words.size() * sizeof(std::uint32_t);
        createInfo.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult status = vkCreateShaderModule(device, &createInfo, nullptr, &module);
        if (status != VK_SUCCESS) error = vkError("vkCreateShaderModule", status);
        return module;
    }

    bool ensureBuffer(Buffer& buffer, const VkDeviceSize requestedSize,
                      const VkBufferUsageFlags usage,
                      const VmaMemoryUsage memoryUsage,
                      const VmaAllocationCreateFlags flags,
                      const VkMemoryPropertyFlags requiredFlags,
                      std::string& error) {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= requestedSize) return true;
        destroyBuffer(buffer);
        return createBuffer(requestedSize, usage, memoryUsage, flags,
                            requiredFlags, buffer, error);
    }

    bool createComputePipeline(VkShaderModule shader, VkPipeline& pipeline,
                               std::string& error) const {
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = stage;
        info.layout = pipelineLayout;
        const VkResult status = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateComputePipelines", status);
            return false;
        }
        return true;
    }

    bool ensureCommonResources(std::string& error) {
        if (descriptorLayout != VK_NULL_HANDLE) return true;
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkResult status = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                                       &descriptorLayout);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateDescriptorSetLayout", status);
            return false;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = 64;
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &descriptorLayout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &pushRange;
        status = vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreatePipelineLayout", status);
            return false;
        }

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        status = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateDescriptorPool", status);
            return false;
        }
        const std::array<VkDescriptorSetLayout, 2> layouts{
            descriptorLayout, descriptorLayout};
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = descriptorPool;
        setInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        setInfo.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 2> descriptorSets{};
        status = vkAllocateDescriptorSets(device, &setInfo, descriptorSets.data());
        if (status != VK_SUCCESS) {
            error = vkError("vkAllocateDescriptorSets", status);
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 2;
        std::array<VkCommandBuffer, 2> commandBuffers{};
        status = vkAllocateCommandBuffers(device, &commandInfo, commandBuffers.data());
        if (status != VK_SUCCESS) {
            error = vkError("vkAllocateCommandBuffers", status);
            return false;
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            FrameSlot& slot = slots[index];
            slot.descriptorSet = descriptorSets[index];
            slot.commandBuffer = commandBuffers[index];
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            status = vkCreateFence(device, &fenceInfo, nullptr, &slot.fence);
            if (status != VK_SUCCESS) {
                error = vkError("vkCreateFence", status);
                return false;
            }
            if (timestampValidBits != 0u) {
                VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                queryInfo.queryCount = 4;
                status = vkCreateQueryPool(device, &queryInfo, nullptr, &slot.queryPool);
                if (status != VK_SUCCESS) {
                    error = vkError("vkCreateQueryPool", status);
                    return false;
                }
            }
            if (!ensureSlotBuffers(slot, 1u, error)) return false;
        }
        updateDiagnosticSnapshot(slots[0]);
        return true;
    }

    bool ensureSlotBuffers(FrameSlot& slot, const std::uint32_t pixelCount,
                           std::string& error) {
        const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(pixelCount) * 4u;
        const VkDeviceSize statsBytes = 8u * sizeof(std::uint32_t);
        const auto gpuUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        const auto mapped = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        const auto uploadFlags = mapped |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        const auto readbackFlags = mapped |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        return ensureBuffer(slot.accumulation, pixelBytes * 3u,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   gpuUsage, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error) &&
               ensureBuffer(slot.resolved, pixelBytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   gpuUsage, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error) &&
               ensureBuffer(slot.settings, sizeof(FractureGpuSettings),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   gpuUsage, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error) &&
               ensureBuffer(slot.statistics, statsBytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   gpuUsage, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error) &&
               ensureBuffer(slot.upload, sizeof(FractureGpuSettings),
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                   uploadFlags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, error) &&
               ensureBuffer(slot.readback, pixelBytes + statsBytes,
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                   readbackFlags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, error);
    }

    bool ensureWallPipelines(std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (tracePipeline != VK_NULL_HANDLE && resolvePipeline != VK_NULL_HANDLE) return true;
        traceShader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                   "first_light.comp.spv", error);
        if (traceShader == VK_NULL_HANDLE) return false;
        resolveShader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                     "resolve.comp.spv", error);
        if (resolveShader == VK_NULL_HANDLE) return false;
        return createComputePipeline(traceShader, tracePipeline, error) &&
               createComputePipeline(resolveShader, resolvePipeline, error);
    }

    bool ensureFractureWallPipeline(std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (fractureTracePipeline != VK_NULL_HANDLE) return true;
        fractureTraceShader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                           "first_light.comp.fracture.spv", error);
        return fractureTraceShader != VK_NULL_HANDLE &&
               createComputePipeline(fractureTraceShader, fractureTracePipeline, error);
    }

    bool ensureLegacyWallPipeline(const ShapeFamily family, std::string& error) {
        if (!ensureWallPipelines(error)) return false;
        const std::uint32_t raw = static_cast<std::uint32_t>(family);
        if (raw < 1u || raw > 5u) return true;
        const std::size_t index = raw - 1u;
        if (familyTracePipelines[index] != VK_NULL_HANDLE) return true;
        familyTraceShaders[index] = createShader(
            std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                ("first_light.comp.family" + std::to_string(raw) + ".spv"), error);
        return familyTraceShaders[index] != VK_NULL_HANDLE &&
               createComputePipeline(familyTraceShaders[index],
                                     familyTracePipelines[index], error);
    }

    bool ensureTrace128Pipeline(const ShapeFamily family, std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (family == ShapeFamily::Fracture) {
            if (fractureTrace128Pipeline != VK_NULL_HANDLE) return true;
            fractureTrace128Shader = createShader(
                std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                    "first_light.comp.group128.fracture.spv", error);
            return fractureTrace128Shader != VK_NULL_HANDLE &&
                   createComputePipeline(fractureTrace128Shader,
                                         fractureTrace128Pipeline, error);
        }
        const std::uint32_t raw = static_cast<std::uint32_t>(family);
        if (raw < 1u || raw > 5u) return false;
        const std::size_t index = raw - 1u;
        if (familyTrace128Pipelines[index] != VK_NULL_HANDLE) return true;
        familyTrace128Shaders[index] = createShader(
            std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                ("first_light.comp.group128.family" + std::to_string(raw) + ".spv"), error);
        return familyTrace128Shaders[index] != VK_NULL_HANDLE &&
               createComputePipeline(familyTrace128Shaders[index],
                                     familyTrace128Pipelines[index], error);
    }

    bool ensureResolve8Pipeline(std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (resolve8Pipeline != VK_NULL_HANDLE) return true;
        resolve8Shader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                      "resolve.comp.group8.spv", error);
        return resolve8Shader != VK_NULL_HANDLE &&
               createComputePipeline(resolve8Shader, resolve8Pipeline, error);
    }

    bool ensurePreviewPipeline(std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (previewPipeline != VK_NULL_HANDLE) return true;
        previewShader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                     "glass_preview.comp.spv", error);
        return previewShader != VK_NULL_HANDLE &&
               createComputePipeline(previewShader, previewPipeline, error);
    }

    bool ensureFracturePreviewPipeline(std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (fracturePreviewPipeline != VK_NULL_HANDLE) return true;
        fracturePreviewShader = createShader(std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                                             "glass_preview.comp.fracture.spv", error);
        return fracturePreviewShader != VK_NULL_HANDLE &&
               createComputePipeline(fracturePreviewShader, fracturePreviewPipeline, error);
    }

    bool ensureLegacyPreviewPipeline(const ShapeFamily family, std::string& error) {
        if (!ensurePreviewPipeline(error)) return false;
        const std::uint32_t raw = static_cast<std::uint32_t>(family);
        if (raw < 1u || raw > 5u) return true;
        const std::size_t index = raw - 1u;
        if (familyPreviewPipelines[index] != VK_NULL_HANDLE) return true;
        familyPreviewShaders[index] = createShader(
            std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                ("glass_preview.comp.family" + std::to_string(raw) + ".spv"), error);
        return familyPreviewShaders[index] != VK_NULL_HANDLE &&
               createComputePipeline(familyPreviewShaders[index],
                                     familyPreviewPipelines[index], error);
    }

    bool ensurePreview8Pipeline(const ShapeFamily family, std::string& error) {
        if (!ensureCommonResources(error)) return false;
        if (family == ShapeFamily::Fracture) {
            if (fracturePreview8Pipeline != VK_NULL_HANDLE) return true;
            fracturePreview8Shader = createShader(
                std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                    "glass_preview.comp.group8.fracture.spv", error);
            return fracturePreview8Shader != VK_NULL_HANDLE &&
                   createComputePipeline(fracturePreview8Shader,
                                         fracturePreview8Pipeline, error);
        }
        const std::uint32_t raw = static_cast<std::uint32_t>(family);
        if (raw < 1u || raw > 5u) return false;
        const std::size_t index = raw - 1u;
        if (familyPreview8Pipelines[index] != VK_NULL_HANDLE) return true;
        familyPreview8Shaders[index] = createShader(
            std::filesystem::path(GLASSLIGHT_SHADER_DIR) /
                ("glass_preview.comp.group8.family" + std::to_string(raw) + ".spv"), error);
        return familyPreview8Shaders[index] != VK_NULL_HANDLE &&
               createComputePipeline(familyPreview8Shaders[index],
                                     familyPreview8Pipelines[index], error);
    }

    void updateDescriptors(const FrameSlot& slot) const {
        const std::array<VkDescriptorBufferInfo, 4> infos{{
            {slot.accumulation.buffer, 0, slot.accumulation.size},
            {slot.resolved.buffer, 0, slot.resolved.size},
            {slot.statistics.buffer, 0, slot.statistics.size},
            {slot.settings.buffer, 0, slot.settingsPayloadSize}
        }};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = slot.descriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    bool beginCommands(FrameSlot& slot, std::string& error) const {
        VkResult status = vkResetFences(device, 1, &slot.fence);
        if (status == VK_SUCCESS) status = vkResetCommandBuffer(slot.commandBuffer, 0);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (status == VK_SUCCESS) status = vkBeginCommandBuffer(slot.commandBuffer, &beginInfo);
        if (status != VK_SUCCESS) {
            error = vkError("Preparing Vulkan command buffer", status);
            return false;
        }
        return true;
    }

    bool submitCommands(FrameSlot& slot, std::string& error) const {
        VkResult status = vkEndCommandBuffer(slot.commandBuffer);
        if (status != VK_SUCCESS) {
            error = vkError("vkEndCommandBuffer", status);
            return false;
        }
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &slot.commandBuffer;
        slot.submittedAt = std::chrono::steady_clock::now();
        status = vkQueueSubmit(queue, 1, &submitInfo, slot.fence);
        if (status != VK_SUCCESS) {
            error = vkError("Vulkan render submission", status);
            return false;
        }
        slot.state = SlotState::Submitted;
        return true;
    }

    void updateDiagnosticSnapshot(const FrameSlot& slot) {
        diagnosticSnapshot.deviceName = selectedGpu;
        diagnosticSnapshot.driverName = driverName;
        diagnosticSnapshot.driverVersion = driverVersion;
        diagnosticSnapshot.timestampAvailable = timestampValidBits != 0u;
        diagnosticSnapshot.timestampPeriodNanoseconds = timestampPeriod;
        const auto describe = [](const Buffer& buffer) {
            MemoryAllocationDiagnostics result;
            result.memoryTypeIndex = buffer.memoryTypeIndex;
            result.deviceLocal = (buffer.memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            result.hostVisible = (buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
            result.hostCached = (buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
            result.hostCoherent = (buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return result;
        };
        diagnosticSnapshot.accumulation = describe(slot.accumulation);
        diagnosticSnapshot.resolved = describe(slot.resolved);
        diagnosticSnapshot.settings = describe(slot.settings);
        diagnosticSnapshot.statistics = describe(slot.statistics);
        diagnosticSnapshot.upload = describe(slot.upload);
        diagnosticSnapshot.readback = describe(slot.readback);
        diagnosticSnapshot.transientAllocationBytes = 0u;
        for (const FrameSlot& frameSlot : slots) {
            diagnosticSnapshot.transientAllocationBytes +=
                frameSlot.accumulation.size + frameSlot.resolved.size +
                frameSlot.settings.size + frameSlot.statistics.size +
                frameSlot.upload.size + frameSlot.readback.size;
        }
    }

    void finishSlot(FrameSlot& slot) {
        const auto now = std::chrono::steady_clock::now();
        slot.timings.submitToReadyMilliseconds =
            std::chrono::duration<double, std::milli>(now - slot.submittedAt).count();
        auto& lastCompletion = slot.glassPreview
            ? lastPreviewCompletion : lastArtworkCompletion;
        if (lastCompletion.time_since_epoch().count() != 0) {
            slot.timings.completedFrameIntervalMilliseconds =
                std::chrono::duration<double, std::milli>(now - lastCompletion).count();
        }
        lastCompletion = now;
        const VkResult invalidateStatus = vmaInvalidateAllocation(
            allocator, slot.readback.allocation, 0, VK_WHOLE_SIZE);
        if (invalidateStatus != VK_SUCCESS) {
            slot.failure = vkError("vmaInvalidateAllocation", invalidateStatus);
            slot.state = SlotState::Failed;
            return;
        }
        const std::uint32_t width = slot.glassPreview
            ? slot.previewRequest.width : slot.request.width;
        const std::uint32_t height = slot.glassPreview
            ? slot.previewRequest.height : slot.request.height;
        const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(width) * height * 4u;
        const auto* gpuStats = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(slot.readback.mapped) + pixelBytes);
        const std::uint32_t dispatchCount = slot.stats.dispatchCount;
        slot.stats = {};
        slot.stats.dispatchCount = dispatchCount;
        slot.stats.requestedSamples = slot.glassPreview ? 0u : slot.request.samples;
        slot.stats.tracedSamples = slot.stats.requestedSamples;
        slot.stats.depositedSamples = slot.glassPreview ? gpuStats[6] : gpuStats[0];
        slot.stats.estimatedHitRate = slot.glassPreview ? 0.0 :
            static_cast<double>(gpuStats[0]) / static_cast<double>(slot.request.samples);
        slot.stats.resolvedFamily = resolveShapeFamily(slot.glassPreview
            ? slot.previewRequest.settings : slot.request.settings);
        slot.stats.sampleOffset = slot.glassPreview ? 0u : slot.request.sampleOffset;
        slot.stats.internalReflections = gpuStats[1];
        slot.stats.interfaceEvents = gpuStats[4];
        slot.stats.traversalExhausted = gpuStats[5];
        slot.stats.endToEndMilliseconds = slot.timings.submitToReadyMilliseconds;
        if (slot.queryPool != VK_NULL_HANDLE) {
            std::array<std::uint64_t, 4> stamps{};
            const VkResult queryStatus = vkGetQueryPoolResults(
                device, slot.queryPool, 0, 4, sizeof(stamps), stamps.data(),
                sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (queryStatus == VK_SUCCESS) {
                const auto elapsed = [&](const std::size_t begin, const std::size_t end) {
                    std::uint64_t ticks = stamps[end] - stamps[begin];
                    if (timestampValidBits < 64u) {
                        ticks &= (std::uint64_t{1} << timestampValidBits) - 1u;
                    }
                    return static_cast<double>(ticks) * timestampPeriod / 1'000'000.0;
                };
                slot.timings.traceMilliseconds = elapsed(0, 1);
                slot.timings.resolveMilliseconds = elapsed(1, 2);
                slot.timings.transferMilliseconds = elapsed(2, 3);
            }
        } else {
            slot.timings.traceMilliseconds = slot.timings.submitToReadyMilliseconds;
        }
        slot.state = SlotState::Ready;
        if (slot.releaseRequested) {
            slot.releaseRequested = false;
            slot.state = SlotState::Free;
        }
    }

    void reapSubmitted() {
        for (FrameSlot& slot : slots) {
            if (slot.state != SlotState::Submitted) continue;
            const VkResult status = vkGetFenceStatus(device, slot.fence);
            if (status == VK_SUCCESS) {
                finishSlot(slot);
            } else if (status != VK_NOT_READY) {
                slot.failure = vkError("vkGetFenceStatus", status);
                slot.state = SlotState::Failed;
            }
        }
    }
};

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    delete impl_;
}

bool VulkanRenderer::initialize(std::string& error) {
    return initialize({}, error);
}

bool VulkanRenderer::initialize(const std::string& preferredGpuName, std::string& error) {
    if (impl_ != nullptr) return true;
    auto impl = std::make_unique<Impl>();

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "GlassLight";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(
        0, GLASSLIGHT_VERSION_MAJOR, GLASSLIGHT_VERSION_MINOR,
        GLASSLIGHT_VERSION_PATCH);
    appInfo.pEngineName = "GlassLight Compute";
    appInfo.engineVersion = appInfo.applicationVersion;
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    const char* validationEnvironment = SDL_GetEnvironmentVariable(
        SDL_GetEnvironment(), "GLASSLIGHT_VULKAN_VALIDATION");
    const bool validationRequested = validationEnvironment &&
        std::string(validationEnvironment) != "0";
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    const char* debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    if (validationRequested) {
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        const bool layerAvailable = std::any_of(layers.begin(), layers.end(),
            [&](const VkLayerProperties& layer) {
                return std::string(layer.layerName) == validationLayer;
            });
        if (!layerAvailable) {
            error = "GLASSLIGHT_VULKAN_VALIDATION was requested, but "
                    "VK_LAYER_KHRONOS_validation is unavailable.";
            return false;
        }
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = &debugExtension;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = validationCallback;
        instanceInfo.pNext = &debugInfo;
    }
    VkResult status = vkCreateInstance(&instanceInfo, nullptr, &impl->instance);
    if (status != VK_SUCCESS) {
        error = vkError("vkCreateInstance", status);
        return false;
    }
    if (validationRequested) {
        const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(impl->instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!createDebug || createDebug(impl->instance, &debugInfo, nullptr,
                                        &impl->debugMessenger) != VK_SUCCESS) {
            error = "Could not create the Vulkan validation messenger.";
            return false;
        }
    }

    std::uint32_t deviceCount = 0;
    status = vkEnumeratePhysicalDevices(impl->instance, &deviceCount, nullptr);
    if (status != VK_SUCCESS || deviceCount == 0) {
        error = "No Vulkan physical devices were found.";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(impl->instance, &deviceCount, devices.data());

    int bestScore = std::numeric_limits<int>::min();
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2) continue;
        if (!preferredGpuName.empty() && preferredGpuName != properties.deviceName) continue;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t family = 0; family < familyCount; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 300 :
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 200 :
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 100 : 0;
            score += (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 ? 10 : 0;
            if (score > bestScore) {
                bestScore = score;
                impl->physicalDevice = candidate;
                impl->queueFamily = family;
                impl->selectedGpu = properties.deviceName;
                impl->maxDispatchX = properties.limits.maxComputeWorkGroupCount[0];
            }
        }
    }
    if (impl->physicalDevice == VK_NULL_HANDLE) {
        error = preferredGpuName.empty()
            ? "No Vulkan 1.2 device with a compute queue is available."
            : "The requested GPU is not available as a Vulkan 1.2 compute device: " +
                preferredGpuName;
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(impl->physicalDevice, &impl->memoryProperties);
    VkPhysicalDeviceDriverProperties driverProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 selectedProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    selectedProperties.pNext = &driverProperties;
    vkGetPhysicalDeviceProperties2(impl->physicalDevice, &selectedProperties);
    impl->driverName = driverProperties.driverName;
    impl->driverVersion = selectedProperties.properties.driverVersion;
    impl->timestampPeriod = selectedProperties.properties.limits.timestampPeriod;
    std::uint32_t selectedFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        impl->physicalDevice, &selectedFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> selectedFamilies(selectedFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        impl->physicalDevice, &selectedFamilyCount, selectedFamilies.data());
    impl->timestampValidBits = selectedFamilies[impl->queueFamily].timestampValidBits;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = impl->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    status = vkCreateDevice(impl->physicalDevice, &deviceInfo, nullptr, &impl->device);
    if (status != VK_SUCCESS) {
        error = vkError("vkCreateDevice", status);
        return false;
    }
    vkGetDeviceQueue(impl->device, impl->queueFamily, 0, &impl->queue);

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorInfo.instance = impl->instance;
    allocatorInfo.physicalDevice = impl->physicalDevice;
    allocatorInfo.device = impl->device;
    status = vmaCreateAllocator(&allocatorInfo, &impl->allocator);
    if (status != VK_SUCCESS) {
        error = vkError("vmaCreateAllocator", status);
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = impl->queueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    status = vkCreateCommandPool(impl->device, &poolInfo, nullptr, &impl->commandPool);
    if (status != VK_SUCCESS) {
        error = vkError("vkCreateCommandPool", status);
        return false;
    }

    impl_ = impl.release();
    return true;
}

std::vector<std::string> VulkanRenderer::compatibleGpuNames(std::string& error) {
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "GlassLight GPU discovery";
    appInfo.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult status = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (status != VK_SUCCESS) {
        error = vkError("vkCreateInstance", status);
        return {};
    }

    std::uint32_t deviceCount = 0;
    status = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (status == VK_SUCCESS && deviceCount > 0) {
        status = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    }
    std::vector<std::string> result;
    if (status == VK_SUCCESS) {
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            const bool compute = std::any_of(families.begin(), families.end(),
                [](const VkQueueFamilyProperties& family) {
                    return (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                });
            if (compute) result.emplace_back(properties.deviceName);
        }
    } else {
        error = vkError("vkEnumeratePhysicalDevices", status);
    }
    vkDestroyInstance(instance, nullptr);
    return result;
}

RenderTicket VulkanRenderer::submit(const RenderRequest& request, std::string& error) {
    if (impl_ == nullptr && !initialize(error)) return {};
    if (request.width == 0 || request.height == 0 || request.samples == 0) {
        error = "Render dimensions and sample count must be nonzero.";
        return {};
    }
    if ((request.traceGroupSize != 128u && request.traceGroupSize != 256u) ||
        (request.imageGroupSize != 8u && request.imageGroupSize != 16u)) {
        error = "Trace workgroups must be 128 or 256, and image workgroups must be 8 or 16.";
        return {};
    }
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(request.width) * request.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "Requested image is too large.";
        return {};
    }
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(pixelCount64);
    const ShapeFamily family = resolveShapeFamily(request.settings);
    std::scoped_lock lock(impl_->mutex);
    impl_->reapSubmitted();
    if (!impl_->ensureWallPipelines(error) ||
        (family == ShapeFamily::Fracture
            ? !impl_->ensureFractureWallPipeline(error)
            : !impl_->ensureLegacyWallPipeline(family, error)) ||
        (request.traceGroupSize == 128u &&
            !impl_->ensureTrace128Pipeline(family, error)) ||
        (request.imageGroupSize == 8u && !impl_->ensureResolve8Pipeline(error))) {
        return {};
    }
    auto slotIt = impl_->slots.end();
    if (request.slotAffinity && *request.slotAffinity < impl_->slots.size()) {
        auto candidate = impl_->slots.begin() + *request.slotAffinity;
        if (candidate->state == Impl::SlotState::Free) slotIt = candidate;
    } else {
        slotIt = std::find_if(impl_->slots.begin(), impl_->slots.end(),
            [](const Impl::FrameSlot& slot) { return slot.state == Impl::SlotState::Free; });
    }
    if (slotIt == impl_->slots.end()) {
        error = "All asynchronous render slots are busy.";
        return {};
    }
    Impl::FrameSlot& slot = *slotIt;
    if (!impl_->ensureSlotBuffers(slot, pixelCount, error)) return {};
    impl_->updateDiagnosticSnapshot(slot);
    const std::uint32_t seed = shaderSeed(request.settings.seed, 1u);
    const PreparedGpuSettings gpuSettings = makeGpuSettings(
        request.settings, family, seed, request.interactiveInterfaceDepth);
    impl_->diagnosticSnapshot.settingsPayloadBytes =
        static_cast<std::uint32_t>(gpuSettings.size);
    slot.settingsPayloadSize = gpuSettings.size;
    std::memcpy(slot.upload.mapped, gpuSettings.bytes.data(), gpuSettings.size);
    const VkResult flushStatus = vmaFlushAllocation(
        impl_->allocator, slot.upload.allocation, 0, gpuSettings.size);
    if (flushStatus != VK_SUCCESS) {
        error = vkError("vmaFlushAllocation", flushStatus);
        return {};
    }
    impl_->updateDescriptors(slot);
    if (!impl_->beginCommands(slot, error)) return {};

    VkCommandBuffer commandBuffer = slot.commandBuffer;
    VkPipelineLayout pipelineLayout = impl_->pipelineLayout;
    VkPipeline resolvePipeline = request.imageGroupSize == 8u
        ? impl_->resolve8Pipeline : impl_->resolvePipeline;
    VkDescriptorSet descriptorSet = slot.descriptorSet;
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, slot.queryPool, 0, 4);
    }
    VkBufferCopy settingsCopy{0, 0, gpuSettings.size};
    vkCmdCopyBuffer(commandBuffer, slot.upload.buffer, slot.settings.buffer,
                    1, &settingsCopy);
    if (!request.preserveAccumulation) {
        vkCmdFillBuffer(commandBuffer, slot.accumulation.buffer, 0,
                        slot.accumulation.size, 0);
    }
    vkCmdFillBuffer(commandBuffer, slot.statistics.buffer, 0,
                    slot.statistics.size, 0);
    std::array<VkBufferMemoryBarrier, 3> clearBarriers{};
    const std::array<VkBuffer, 3> clearedBuffers{
        slot.settings.buffer, slot.statistics.buffer, slot.accumulation.buffer};
    const std::size_t clearBarrierCount = request.preserveAccumulation ? 2u : 3u;
    for (std::size_t index = 0; index < clearBarrierCount; ++index) {
        clearBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        clearBarriers[index].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT;
        clearBarriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarriers[index].buffer = clearedBuffers[index];
        clearBarriers[index].offset = 0;
        clearBarriers[index].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
        static_cast<std::uint32_t>(clearBarrierCount), clearBarriers.data(), 0, nullptr);
    if (request.preserveAccumulation) {
        VkBufferMemoryBarrier preserveBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        preserveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        preserveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT;
        preserveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preserveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preserveBarrier.buffer = slot.accumulation.buffer;
        preserveBarrier.offset = 0;
        preserveBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
            1, &preserveBarrier, 0, nullptr);
    }

    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            slot.queryPool, 0);
    }

    const std::size_t familyIndex = static_cast<std::uint32_t>(family) - 1u;
    const VkPipeline tracePipeline = request.traceGroupSize == 128u
        ? (family == ShapeFamily::Fracture
            ? impl_->fractureTrace128Pipeline
            : impl_->familyTrace128Pipelines[familyIndex])
        : (family == ShapeFamily::Fracture
            ? impl_->fractureTracePipeline
            : impl_->familyTracePipelines[familyIndex]);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    const std::uint64_t maximumBatch =
        static_cast<std::uint64_t>(impl_->maxDispatchX) * request.traceGroupSize;
    std::uint64_t traced = 0;
    std::uint32_t dispatchCount = 0;
    while (traced < request.samples) {
        const std::uint32_t batch = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            request.samples - traced, maximumBatch));
        const std::uint64_t base = request.sampleOffset + traced;
        const TracePush tracePush{
            request.width, request.height, batch,
            static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32u),
            request.samples, request.progressivePass, request.exportRender ? 1u : 0u
        };
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(tracePush), &tracePush);
        vkCmdDispatch(commandBuffer,
                      (batch + request.traceGroupSize - 1u) / request.traceGroupSize, 1, 1);
        traced += batch;
        ++dispatchCount;
    }
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            slot.queryPool, 1);
    }
    VkBufferMemoryBarrier traceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    traceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    traceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    traceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    traceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    traceBarrier.buffer = slot.accumulation.buffer;
    traceBarrier.offset = 0;
    traceBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
        1, &traceBarrier, 0, nullptr);

    const auto wall = wallColor(request.settings.wall);
    const ResolvePush resolvePush{
        request.width, request.height,
        request.resolveSamples == 0u ? request.samples : request.resolveSamples, 0,
        request.exportRender ? 1.04f : 1.0f, wall[0], wall[1], wall[2],
        request.settings.wallGrain, request.settings.projectionStrength,
        seed, static_cast<std::uint32_t>(request.settings.wall)
    };
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeline);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(resolvePush), &resolvePush);
    vkCmdDispatch(commandBuffer,
                  (request.width + request.imageGroupSize - 1u) / request.imageGroupSize,
                  (request.height + request.imageGroupSize - 1u) / request.imageGroupSize, 1);
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            slot.queryPool, 2);
    }
    std::array<VkBufferMemoryBarrier, 2> copyBarriers{};
    const std::array<VkBuffer, 2> copiedBuffers{
        slot.resolved.buffer, slot.statistics.buffer};
    for (std::size_t index = 0; index < copyBarriers.size(); ++index) {
        copyBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        copyBarriers[index].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        copyBarriers[index].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copyBarriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyBarriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyBarriers[index].buffer = copiedBuffers[index];
        copyBarriers[index].offset = 0;
        copyBarriers[index].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
        static_cast<std::uint32_t>(copyBarriers.size()), copyBarriers.data(), 0, nullptr);
    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(pixelCount) * 4u;
    const VkBufferCopy pixelCopy{0, 0, pixelBytes};
    const VkBufferCopy statsCopy{0, pixelBytes, 8u * sizeof(std::uint32_t)};
    vkCmdCopyBuffer(commandBuffer, slot.resolved.buffer, slot.readback.buffer,
                    1, &pixelCopy);
    vkCmdCopyBuffer(commandBuffer, slot.statistics.buffer, slot.readback.buffer,
                    1, &statsCopy);
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            slot.queryPool, 3);
    }
    VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = slot.readback.buffer;
    hostBarrier.offset = 0;
    hostBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);

    slot.generation = ++impl_->latestGeneration;
    impl_->latestArtworkGeneration = slot.generation;
    slot.glassPreview = false;
    slot.request = request;
    slot.stats = {};
    slot.stats.dispatchCount = dispatchCount;
    slot.timings = {};
    slot.failure.clear();
    slot.releaseRequested = false;
    if (!impl_->submitCommands(slot, error)) {
        slot.state = Impl::SlotState::Free;
        return {};
    }
    error.clear();
    return {static_cast<std::uint32_t>(slotIt - impl_->slots.begin()), slot.generation};
}

RenderPollResult VulkanRenderer::poll(const RenderTicket ticket) {
    RenderPollResult result;
    if (impl_ == nullptr || !ticket || ticket.slot >= impl_->slots.size()) {
        result.state = RenderPollState::Failed;
        result.error = "Invalid render ticket.";
        return result;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->reapSubmitted();
    Impl::FrameSlot& slot = impl_->slots[ticket.slot];
    if (slot.generation != ticket.generation) {
        result.state = RenderPollState::Failed;
        result.error = "Render ticket has expired.";
        return result;
    }
    const std::uint64_t latestForKind = slot.glassPreview
        ? impl_->latestPreviewGeneration : impl_->latestArtworkGeneration;
    if (ticket.generation < latestForKind) {
        result.state = RenderPollState::Superseded;
        return result;
    }
    if (slot.state == Impl::SlotState::Submitted) return result;
    if (slot.state == Impl::SlotState::Failed) {
        result.state = RenderPollState::Failed;
        result.error = slot.failure;
        return result;
    }
    if (slot.state != Impl::SlotState::Ready) {
        result.state = RenderPollState::Failed;
        result.error = "Render ticket no longer owns a completed frame.";
        return result;
    }
    result.state = RenderPollState::Ready;
    result.frame.rgba8 = static_cast<const std::uint32_t*>(slot.readback.mapped);
    result.frame.width = slot.glassPreview
        ? slot.previewRequest.width : slot.request.width;
    result.frame.height = slot.glassPreview
        ? slot.previewRequest.height : slot.request.height;
    result.frame.pitchBytes = static_cast<std::size_t>(result.frame.width) * 4u;
    result.timings = slot.timings;
    result.stats = slot.stats;
    return result;
}

void VulkanRenderer::release(const RenderTicket ticket) {
    if (impl_ == nullptr || !ticket || ticket.slot >= impl_->slots.size()) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->reapSubmitted();
    Impl::FrameSlot& slot = impl_->slots[ticket.slot];
    if (slot.generation != ticket.generation) return;
    if (slot.state == Impl::SlotState::Submitted) {
        slot.releaseRequested = true;
    } else {
        slot.state = Impl::SlotState::Free;
        slot.releaseRequested = false;
    }
}

void VulkanRenderer::recordSdlUpload(const RenderTicket ticket,
                                     const double milliseconds) {
    if (impl_ == nullptr || !ticket || ticket.slot >= impl_->slots.size()) return;
    std::scoped_lock lock(impl_->mutex);
    Impl::FrameSlot& slot = impl_->slots[ticket.slot];
    if (slot.generation == ticket.generation) {
        slot.timings.sdlUploadMilliseconds = std::max(milliseconds, 0.0);
    }
}

void VulkanRenderer::trimTransientResources() {
    if (impl_ == nullptr) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->reapSubmitted();
    for (Impl::FrameSlot& slot : impl_->slots) {
        if (slot.state != Impl::SlotState::Free) continue;
        impl_->destroyBuffer(slot.readback);
        impl_->destroyBuffer(slot.upload);
        impl_->destroyBuffer(slot.statistics);
        impl_->destroyBuffer(slot.settings);
        impl_->destroyBuffer(slot.resolved);
        impl_->destroyBuffer(slot.accumulation);
        std::string ignored;
        impl_->ensureSlotBuffers(slot, 1u, ignored);
    }
    impl_->updateDiagnosticSnapshot(impl_->slots[0]);
}

bool VulkanRenderer::render(const RenderRequest& request, RenderedImage& output,
                            RenderStats& stats, std::string& error) {
    const auto callStart = std::chrono::steady_clock::now();
    RenderTicket ticket;
    while (!ticket) {
        ticket = submit(request, error);
        if (!ticket && error == "All asynchronous render slots are busy.") {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (!ticket) {
            return false;
        }
    }
    while (true) {
        RenderPollResult result = poll(ticket);
        if (result.state == RenderPollState::Pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (result.state == RenderPollState::Superseded) {
            release(ticket);
            error = "Blocking render was superseded by a newer request.";
            return false;
        }
        if (result.state == RenderPollState::Failed) {
            error = result.error;
            release(ticket);
            return false;
        }
        output.width = result.frame.width;
        output.height = result.frame.height;
        output.gpuName = impl_->selectedGpu;
        output.renderMilliseconds = result.timings.traceMilliseconds +
                                    result.timings.resolveMilliseconds +
                                    result.timings.transferMilliseconds;
        const std::size_t pixelCount = static_cast<std::size_t>(output.width) * output.height;
        output.rgba8.resize(pixelCount);
        std::memcpy(output.rgba8.data(), result.frame.rgba8,
                    pixelCount * sizeof(std::uint32_t));
        output.nonBackgroundPixels = std::min<std::uint64_t>(
            pixelCount, result.stats.depositedSamples);
        stats = result.stats;
        stats.endToEndMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - callStart).count();
        release(ticket);
        error.clear();
        return true;
    }
}

bool VulkanRenderer::render(const RenderRequest& request, RenderedImage& output,
                            std::string& error) {
    RenderStats ignored;
    return render(request, output, ignored, error);
}

RenderTicket VulkanRenderer::submitGlassPreview(const GlassPreviewRequest& request,
                                                std::string& error) {
    if (impl_ == nullptr && !initialize(error)) return {};
    if (request.width == 0 || request.height == 0) {
        error = "Glass preview dimensions must be nonzero.";
        return {};
    }
    if (request.imageGroupSize != 8u && request.imageGroupSize != 16u) {
        error = "Preview image workgroups must be 8 or 16.";
        return {};
    }
    const std::uint64_t pixelCount64 =
        static_cast<std::uint64_t>(request.width) * request.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "Requested glass preview is too large.";
        return {};
    }
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(pixelCount64);
    const ShapeFamily family = resolveShapeFamily(request.settings);
    std::scoped_lock lock(impl_->mutex);
    impl_->reapSubmitted();
    if (!impl_->ensurePreviewPipeline(error) ||
        (family == ShapeFamily::Fracture
            ? !impl_->ensureFracturePreviewPipeline(error)
            : !impl_->ensureLegacyPreviewPipeline(family, error)) ||
        (request.imageGroupSize == 8u &&
            !impl_->ensurePreview8Pipeline(family, error))) {
        return {};
    }
    auto slotIt = impl_->slots.begin() + 1;
    if (slotIt->state != Impl::SlotState::Free) slotIt = impl_->slots.end();
    if (slotIt == impl_->slots.end()) {
        error = "All asynchronous render slots are busy.";
        return {};
    }
    Impl::FrameSlot& slot = *slotIt;
    if (!impl_->ensureSlotBuffers(slot, pixelCount, error)) return {};
    impl_->updateDiagnosticSnapshot(slot);
    const std::uint32_t seed = shaderSeed(request.settings.seed, 1u);
    const PreparedGpuSettings gpuSettings = makeGpuSettings(request.settings, family, seed);
    impl_->diagnosticSnapshot.settingsPayloadBytes =
        static_cast<std::uint32_t>(gpuSettings.size);
    slot.settingsPayloadSize = gpuSettings.size;
    std::memcpy(slot.upload.mapped, gpuSettings.bytes.data(), gpuSettings.size);
    const VkResult flushStatus = vmaFlushAllocation(
        impl_->allocator, slot.upload.allocation, 0, gpuSettings.size);
    if (flushStatus != VK_SUCCESS) {
        error = vkError("vmaFlushAllocation", flushStatus);
        return {};
    }
    impl_->updateDescriptors(slot);
    if (!impl_->beginCommands(slot, error)) return {};

    VkCommandBuffer commandBuffer = slot.commandBuffer;
    if (slot.queryPool != VK_NULL_HANDLE) vkCmdResetQueryPool(commandBuffer, slot.queryPool, 0, 4);
    const VkBufferCopy settingsCopy{0, 0, gpuSettings.size};
    vkCmdCopyBuffer(commandBuffer, slot.upload.buffer, slot.settings.buffer, 1, &settingsCopy);
    vkCmdFillBuffer(commandBuffer, slot.statistics.buffer, 0, slot.statistics.size, 0);
    std::array<VkBufferMemoryBarrier, 2> clearBarriers{};
    const std::array<VkBuffer, 2> clearBuffers{slot.settings.buffer, slot.statistics.buffer};
    for (std::size_t index = 0; index < clearBarriers.size(); ++index) {
        clearBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        clearBarriers[index].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT;
        clearBarriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarriers[index].buffer = clearBuffers[index];
        clearBarriers[index].offset = 0;
        clearBarriers[index].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
        static_cast<std::uint32_t>(clearBarriers.size()), clearBarriers.data(), 0, nullptr);
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            slot.queryPool, 0);
    }

    const PreviewPush push{
        request.width, request.height, request.edgeOverlay ? 1u : 0u,
        request.interactive ? 1u : 0u,
        request.orbitYaw, request.orbitPitch, request.zoom, 0.0f
    };
    const std::size_t familyIndex = static_cast<std::uint32_t>(family) - 1u;
    const VkPipeline previewPipeline = request.imageGroupSize == 8u
        ? (family == ShapeFamily::Fracture
            ? impl_->fracturePreview8Pipeline
            : impl_->familyPreview8Pipelines[familyIndex])
        : (family == ShapeFamily::Fracture
            ? impl_->fracturePreviewPipeline
            : impl_->familyPreviewPipelines[familyIndex]);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, previewPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        impl_->pipelineLayout, 0, 1, &slot.descriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, impl_->pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer,
                  (request.width + request.imageGroupSize - 1u) / request.imageGroupSize,
                  (request.height + request.imageGroupSize - 1u) / request.imageGroupSize, 1);
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            slot.queryPool, 1);
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            slot.queryPool, 2);
    }
    std::array<VkBufferMemoryBarrier, 2> copyBarriers{};
    const std::array<VkBuffer, 2> copyBuffers{slot.resolved.buffer, slot.statistics.buffer};
    for (std::size_t index = 0; index < copyBarriers.size(); ++index) {
        copyBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        copyBarriers[index].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        copyBarriers[index].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copyBarriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyBarriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyBarriers[index].buffer = copyBuffers[index];
        copyBarriers[index].offset = 0;
        copyBarriers[index].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
        static_cast<std::uint32_t>(copyBarriers.size()), copyBarriers.data(), 0, nullptr);
    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(pixelCount) * 4u;
    const VkBufferCopy pixelCopy{0, 0, pixelBytes};
    const VkBufferCopy statsCopy{0, pixelBytes, 8u * sizeof(std::uint32_t)};
    vkCmdCopyBuffer(commandBuffer, slot.resolved.buffer, slot.readback.buffer, 1, &pixelCopy);
    vkCmdCopyBuffer(commandBuffer, slot.statistics.buffer, slot.readback.buffer, 1, &statsCopy);
    if (slot.queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            slot.queryPool, 3);
    }
    VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = slot.readback.buffer;
    hostBarrier.offset = 0;
    hostBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);

    slot.generation = ++impl_->latestGeneration;
    impl_->latestPreviewGeneration = slot.generation;
    slot.glassPreview = true;
    slot.previewRequest = request;
    slot.stats = {};
    slot.timings = {};
    slot.failure.clear();
    slot.releaseRequested = false;
    if (!impl_->submitCommands(slot, error)) {
        slot.state = Impl::SlotState::Free;
        return {};
    }
    error.clear();
    return {static_cast<std::uint32_t>(slotIt - impl_->slots.begin()), slot.generation};
}

bool VulkanRenderer::renderGlassPreview(const GlassPreviewRequest& request,
                                        RenderedImage& output,
                                        GlassPreviewStats& stats,
                                        std::string& error) {
    const auto callStart = std::chrono::steady_clock::now();
    RenderTicket ticket;
    while (!ticket) {
        ticket = submitGlassPreview(request, error);
        if (!ticket && error == "All asynchronous render slots are busy.") {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (!ticket) return false;
    }
    while (true) {
        RenderPollResult result = poll(ticket);
        if (result.state == RenderPollState::Pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (result.state != RenderPollState::Ready) {
            error = result.state == RenderPollState::Failed
                ? result.error : "Glass preview was superseded.";
            release(ticket);
            return false;
        }
        const std::size_t pixelCount = static_cast<std::size_t>(
            result.frame.width) * result.frame.height;
        output.width = result.frame.width;
        output.height = result.frame.height;
        output.gpuName = impl_->selectedGpu;
        output.renderMilliseconds = result.timings.traceMilliseconds +
                                    result.timings.transferMilliseconds;
        output.rgba8.resize(pixelCount);
        std::memcpy(output.rgba8.data(), result.frame.rgba8,
                    pixelCount * sizeof(std::uint32_t));
        output.nonBackgroundPixels = std::min<std::uint64_t>(
            pixelCount, result.stats.depositedSamples);
        stats = {};
        stats.resolvedFamily = result.stats.resolvedFamily;
        stats.shadedPixels = result.stats.depositedSamples;
        stats.endToEndMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - callStart).count();
        release(ticket);
        error.clear();
        return true;
    }
}

bool VulkanRenderer::renderGlassPreview(const GlassPreviewRequest& request,
                                        RenderedImage& output,
                                        std::string& error) {
    GlassPreviewStats ignored;
    return renderGlassPreview(request, output, ignored, error);
}

bool VulkanRenderer::renderFirstLight(const FirstLightRequest& request, RenderedImage& output,
                                      std::string& error) {
    RenderRequest full;
    full.width = request.width;
    full.height = request.height;
    full.samples = request.samples;
    full.settings.seed = request.seed;
    full.settings.shapeFamily = ShapeFamily::Pebble;
    full.settings.ior = request.ior;
    full.settings.roughness = request.roughness;
    full.settings.lightX = request.lightX;
    full.settings.lightY = request.lightY;
    full.settings.lightIntensity = request.intensity;
    full.settings.projectionStrength = request.causticStrength;
    full.settings.phase = request.phase;
    return render(full, output, error);
}

const std::string& VulkanRenderer::gpuName() const {
    static const std::string empty;
    return impl_ != nullptr ? impl_->selectedGpu : empty;
}

RendererDiagnostics VulkanRenderer::diagnostics() const {
    if (impl_ == nullptr) return {};
    std::scoped_lock lock(impl_->mutex);
    return impl_->diagnosticSnapshot;
}

bool savePng(const RenderedImage& image, const std::filesystem::path& path,
             std::string& error) {
    if (image.width == 0 || image.height == 0 ||
        image.rgba8.size() != static_cast<std::size_t>(image.width) * image.height) {
        error = "Cannot save an empty or malformed image.";
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Could not create image folder: " + ec.message();
        return false;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(image.width),
        static_cast<int>(image.height), SDL_PIXELFORMAT_RGBA32,
        const_cast<std::uint32_t*>(image.rgba8.data()),
        static_cast<int>(image.width * sizeof(std::uint32_t)));
    if (surface == nullptr) {
        error = std::string("Could not create PNG surface: ") + SDL_GetError();
        return false;
    }
    const std::string pathUtf8 = platform::pathToUtf8(path);
    const bool saved = SDL_SavePNG(surface, pathUtf8.c_str());
    if (!saved) error = std::string("Could not save PNG: ") + SDL_GetError();
    SDL_DestroySurface(surface);
    return saved;
}

} // namespace glasslight
