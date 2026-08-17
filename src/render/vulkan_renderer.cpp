#include "render/vulkan_renderer.hpp"
#include "core/fracture_layout.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifndef GLASSLIGHT_SHADER_DIR
#define GLASSLIGHT_SHADER_DIR "shaders"
#endif

namespace glasslight {
namespace {

std::string vkError(const char* operation, VkResult result) {
    return std::string(operation) + " failed with VkResult " + std::to_string(result) + ".";
}

std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open shader: " + path.string();
        return {};
    }
    const std::streamsize bytes = input.tellg();
    if (bytes <= 0 || bytes % 4 != 0) {
        error = "Shader is empty or not valid SPIR-V: " + path.string();
        return {};
    }
    input.seekg(0);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4u);
    if (!input.read(reinterpret_cast<char*>(words.data()), bytes)) {
        error = "Could not read shader: " + path.string();
        return {};
    }
    return words;
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
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
    std::uint32_t padding;
    float orbitYaw;
    float orbitPitch;
    float zoom;
    float padding2;
};

struct GpuSettings {
    // vec4-aligned layout shared with the shader's std430 storage block.
    std::array<std::array<float, 4>, 5> palette{};
    std::array<std::uint32_t, 4> shapeRegion{};
    std::array<float, 4> geometry{};
    std::array<float, 4> optical{};
    std::array<float, 4> light{};
    std::array<float, 4> rotation{};
    std::array<std::uint32_t, 4> renderOptions{};
    std::array<std::uint32_t, 4> fractureInfo{};
    std::array<float, 4> fractureBounds{};
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
static_assert(sizeof(GpuSettings::Shard) == 80);
static_assert(sizeof(GpuSettings) == 4816);

std::array<float, 3> wallColor(WallPreset wall) {
    switch (wall) {
    case WallPreset::NeutralGallery: return {0.90f, 0.90f, 0.88f};
    case WallPreset::CoolPlaster: return {0.78f, 0.84f, 0.88f};
    case WallPreset::Charcoal: return {0.10f, 0.115f, 0.13f};
    case WallPreset::WarmWhitePaint:
    default: return {0.93f, 0.90f, 0.84f};
    }
}

GpuSettings makeGpuSettings(const CompositionSettings& settings,
                            ShapeFamily family, std::uint32_t seed) {
    GpuSettings gpu{};
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
    gpu.rotation = {
        cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch), settings.phase
    };
    gpu.renderOptions = {static_cast<std::uint32_t>(settings.quality), 0u, 0u, 0u};
    if (family == ShapeFamily::Fracture) {
        const FractureLayout layout = buildFractureLayout(settings);
        gpu.fractureInfo = {
            layout.shardCount,
            static_cast<std::uint32_t>(layout.morphology),
            static_cast<std::uint32_t>(layout.motion),
            0u
        };
        gpu.fractureBounds = {layout.boundsCenter[0], layout.boundsCenter[1],
                              layout.boundsCenter[2], layout.boundsRadius};
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
    }
    return gpu;
}

} // namespace

struct VulkanRenderer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    std::uint32_t maxDispatchX = 65535;
    std::string selectedGpu;

    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkShaderModule traceShader = VK_NULL_HANDLE;
    VkShaderModule fractureTraceShader = VK_NULL_HANDLE;
    VkShaderModule resolveShader = VK_NULL_HANDLE;
    VkShaderModule previewShader = VK_NULL_HANDLE;
    VkShaderModule fracturePreviewShader = VK_NULL_HANDLE;
    VkPipeline tracePipeline = VK_NULL_HANDLE;
    VkPipeline fractureTracePipeline = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE;
    VkPipeline previewPipeline = VK_NULL_HANDLE;
    VkPipeline fracturePreviewPipeline = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer accumulation;
    Buffer resolved;
    Buffer settingsBuffer;
    Buffer statsBuffer;

    ~Impl() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        destroyBuffer(statsBuffer);
        destroyBuffer(settingsBuffer);
        destroyBuffer(resolved);
        destroyBuffer(accumulation);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (fracturePreviewPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fracturePreviewPipeline, nullptr);
        if (previewPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, previewPipeline, nullptr);
        if (resolvePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, resolvePipeline, nullptr);
        if (fractureTracePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, fractureTracePipeline, nullptr);
        if (tracePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, tracePipeline, nullptr);
        if (fracturePreviewShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fracturePreviewShader, nullptr);
        if (previewShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, previewShader, nullptr);
        if (resolveShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, resolveShader, nullptr);
        if (fractureTraceShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, fractureTraceShader, nullptr);
        if (traceShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, traceShader, nullptr);
        if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    std::optional<std::uint32_t> findMemoryType(std::uint32_t bits,
                                                VkMemoryPropertyFlags required) const {
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((bits & (1u << index)) != 0u &&
                (memoryProperties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        return std::nullopt;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& result,
                      std::string& error) const {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult status = vkCreateBuffer(device, &bufferInfo, nullptr, &result.buffer);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateBuffer", status);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        const auto memoryType = findMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!memoryType) {
            error = "The selected GPU has no host-visible coherent storage-buffer memory type.";
            vkDestroyBuffer(device, result.buffer, nullptr);
            result.buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memoryType;
        status = vkAllocateMemory(device, &allocation, nullptr, &result.memory);
        if (status != VK_SUCCESS) {
            error = vkError("vkAllocateMemory", status);
            vkDestroyBuffer(device, result.buffer, nullptr);
            result.buffer = VK_NULL_HANDLE;
            return false;
        }
        status = vkBindBufferMemory(device, result.buffer, result.memory, 0);
        if (status != VK_SUCCESS) {
            error = vkError("vkBindBufferMemory", status);
            destroyBuffer(result);
            return false;
        }
        status = vkMapMemory(device, result.memory, 0, size, 0, &result.mapped);
        if (status != VK_SUCCESS) {
            error = vkError("vkMapMemory", status);
            destroyBuffer(result);
            return false;
        }
        result.size = size;
        return true;
    }

    void destroyBuffer(Buffer& buffer) const {
        if (buffer.mapped != nullptr) vkUnmapMemory(device, buffer.memory);
        if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer.buffer, nullptr);
        if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }

    VkShaderModule createShader(const std::filesystem::path& path, std::string& error) const {
        std::vector<std::uint32_t> words = readSpirv(path, error);
        if (words.empty()) {
            const char* base = SDL_GetBasePath();
            if (base != nullptr) {
                const auto fileName = path.filename();
                const std::array<std::filesystem::path, 2> installedCandidates{{
                    std::filesystem::path(base) / ".." / "share" / "glasslight" /
                        "shaders" / fileName,
                    std::filesystem::path(base) / "shaders" / fileName
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

    bool ensureBuffer(Buffer& buffer, VkDeviceSize requestedSize,
                      VkBufferUsageFlags usage, std::string& error) {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= requestedSize) return true;
        destroyBuffer(buffer);
        return createBuffer(requestedSize, usage, buffer, error);
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

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        status = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateDescriptorPool", status);
            return false;
        }
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = descriptorPool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &descriptorLayout;
        status = vkAllocateDescriptorSets(device, &setInfo, &descriptorSet);
        if (status != VK_SUCCESS) {
            error = vkError("vkAllocateDescriptorSets", status);
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        status = vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer);
        if (status != VK_SUCCESS) {
            error = vkError("vkAllocateCommandBuffers", status);
            return false;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        status = vkCreateFence(device, &fenceInfo, nullptr, &fence);
        if (status != VK_SUCCESS) {
            error = vkError("vkCreateFence", status);
            return false;
        }
        return ensureBuffer(accumulation, sizeof(std::uint32_t),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, error) &&
               ensureBuffer(resolved, sizeof(std::uint32_t),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) &&
               ensureBuffer(settingsBuffer, sizeof(GpuSettings),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) &&
               ensureBuffer(statsBuffer, 8u * sizeof(std::uint32_t),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, error);
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

    void updateDescriptors() const {
        const std::array<VkDescriptorBufferInfo, 4> infos{{
            {accumulation.buffer, 0, accumulation.size},
            {resolved.buffer, 0, resolved.size},
            {statsBuffer.buffer, 0, statsBuffer.size},
            {settingsBuffer.buffer, 0, settingsBuffer.size}
        }};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    bool beginCommands(std::string& error) const {
        VkResult status = vkResetFences(device, 1, &fence);
        if (status == VK_SUCCESS) status = vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (status == VK_SUCCESS) status = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (status != VK_SUCCESS) {
            error = vkError("Preparing Vulkan command buffer", status);
            return false;
        }
        return true;
    }

    bool submitCommands(double& gpuMilliseconds, std::string& error) const {
        VkResult status = vkEndCommandBuffer(commandBuffer);
        if (status != VK_SUCCESS) {
            error = vkError("vkEndCommandBuffer", status);
            return false;
        }
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        const auto start = std::chrono::steady_clock::now();
        status = vkQueueSubmit(queue, 1, &submitInfo, fence);
        if (status == VK_SUCCESS) {
            status = vkWaitForFences(device, 1, &fence, VK_TRUE,
                                     std::numeric_limits<std::uint64_t>::max());
        }
        const auto finish = std::chrono::steady_clock::now();
        gpuMilliseconds = std::chrono::duration<double, std::milli>(finish - start).count();
        if (status != VK_SUCCESS) {
            error = vkError("Vulkan render submission", status);
            return false;
        }
        return true;
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
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 2, 0);
    appInfo.pEngineName = "GlassLight Compute";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    VkResult status = vkCreateInstance(&instanceInfo, nullptr, &impl->instance);
    if (status != VK_SUCCESS) {
        error = vkError("vkCreateInstance", status);
        return false;
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

bool VulkanRenderer::render(const RenderRequest& request, RenderedImage& output,
                            RenderStats& stats, std::string& error) {
    const auto callStart = std::chrono::steady_clock::now();
    if (impl_ == nullptr && !initialize(error)) return false;
    if (request.width == 0 || request.height == 0 || request.samples == 0) {
        error = "Render dimensions and sample count must be nonzero.";
        return false;
    }

    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(request.width) * request.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "Requested image is too large.";
        return false;
    }
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(pixelCount64);
    const ShapeFamily family = resolveShapeFamily(request.settings);
    if (!impl_->ensureWallPipelines(error) ||
        (family == ShapeFamily::Fracture && !impl_->ensureFractureWallPipeline(error)) ||
        !impl_->ensureBuffer(impl_->accumulation,
            static_cast<VkDeviceSize>(pixelCount) * 3u * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, error) ||
        !impl_->ensureBuffer(impl_->resolved,
            static_cast<VkDeviceSize>(pixelCount) * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error)) {
        return false;
    }

    const std::uint32_t seed = shaderSeed(request.settings.seed, 1u);
    const GpuSettings gpuSettings = makeGpuSettings(request.settings, family, seed);
    std::memcpy(impl_->settingsBuffer.mapped, &gpuSettings, sizeof(gpuSettings));
    impl_->updateDescriptors();

    VkCommandBuffer commandBuffer = impl_->commandBuffer;
    VkPipelineLayout pipelineLayout = impl_->pipelineLayout;
    VkPipeline tracePipeline = impl_->tracePipeline;
    VkPipeline resolvePipeline = impl_->resolvePipeline;
    VkDescriptorSet descriptorSet = impl_->descriptorSet;
    Buffer& accumulation = impl_->accumulation;
    Buffer& resolved = impl_->resolved;
    Buffer& statsBuffer = impl_->statsBuffer;
    if (!impl_->beginCommands(error)) return false;
    vkCmdFillBuffer(commandBuffer, accumulation.buffer, 0, accumulation.size, 0);
    vkCmdFillBuffer(commandBuffer, statsBuffer.buffer, 0, statsBuffer.size, 0);
    VkMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clearBarrier, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      family == ShapeFamily::Fracture
                          ? impl_->fractureTracePipeline : tracePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    const std::uint64_t maximumBatch = static_cast<std::uint64_t>(impl_->maxDispatchX) * 256u;
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
        vkCmdDispatch(commandBuffer, (batch + 255u) / 256u, 1, 1);
        traced += batch;
        ++dispatchCount;
    }

    VkMemoryBarrier traceBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    traceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    traceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &traceBarrier, 0, nullptr, 0, nullptr);

    const auto wall = wallColor(request.settings.wall);
    const ResolvePush resolvePush{
        request.width, request.height, request.samples, 0,
        request.exportRender ? 1.04f : 1.0f, wall[0], wall[1], wall[2],
        request.settings.wallGrain, request.settings.projectionStrength,
        seed, static_cast<std::uint32_t>(request.settings.wall)
    };
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeline);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(resolvePush), &resolvePush);
    vkCmdDispatch(commandBuffer, (request.width + 15u) / 16u,
                  (request.height + 15u) / 16u, 1);

    VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
    double gpuMilliseconds = 0.0;
    if (!impl_->submitCommands(gpuMilliseconds, error)) return false;

    output.width = request.width;
    output.height = request.height;
    output.gpuName = impl_->selectedGpu;
    output.renderMilliseconds = gpuMilliseconds;
    output.rgba8.resize(pixelCount);
    std::memcpy(output.rgba8.data(), resolved.mapped,
                static_cast<std::size_t>(pixelCount) * sizeof(std::uint32_t));
    const auto* gpuStats = static_cast<const std::uint32_t*>(statsBuffer.mapped);
    // The exact CPU scan was a redundant second pass over the entire readback.
    // Deposited photons are a conservative usefulness proxy for smoke/UI
    // reporting and keep the render handoff within the interactive budget.
    output.nonBackgroundPixels = std::min<std::uint64_t>(pixelCount, gpuStats[0]);
    stats = {};
    stats.requestedSamples = request.samples;
    stats.tracedSamples = request.samples;
    stats.depositedSamples = gpuStats[0];
    stats.dispatchCount = dispatchCount;
    stats.estimatedHitRate = static_cast<double>(stats.depositedSamples) /
        static_cast<double>(request.samples);
    stats.resolvedFamily = family;
    stats.sampleOffset = request.sampleOffset;
    stats.internalReflections = gpuStats[1];
    stats.interfaceEvents = gpuStats[4];
    stats.traversalExhausted = gpuStats[5];
    stats.endToEndMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - callStart).count();

    return true;
}

bool VulkanRenderer::render(const RenderRequest& request, RenderedImage& output,
                            std::string& error) {
    RenderStats ignored;
    return render(request, output, ignored, error);
}

bool VulkanRenderer::renderGlassPreview(const GlassPreviewRequest& request,
                                        RenderedImage& output,
                                        GlassPreviewStats& stats,
                                        std::string& error) {
    const auto callStart = std::chrono::steady_clock::now();
    if (impl_ == nullptr && !initialize(error)) return false;
    if (request.width == 0 || request.height == 0) {
        error = "Glass preview dimensions must be nonzero.";
        return false;
    }
    const std::uint64_t pixelCount64 =
        static_cast<std::uint64_t>(request.width) * request.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "Requested glass preview is too large.";
        return false;
    }
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(pixelCount64);
    const ShapeFamily family = resolveShapeFamily(request.settings);
    if (!impl_->ensurePreviewPipeline(error) ||
        (family == ShapeFamily::Fracture && !impl_->ensureFracturePreviewPipeline(error)) ||
        !impl_->ensureBuffer(impl_->resolved,
            static_cast<VkDeviceSize>(pixelCount) * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error)) {
        return false;
    }

    const std::uint32_t seed = shaderSeed(request.settings.seed, 1u);
    const GpuSettings gpuSettings = makeGpuSettings(request.settings, family, seed);
    std::memcpy(impl_->settingsBuffer.mapped, &gpuSettings, sizeof(gpuSettings));
    impl_->updateDescriptors();
    if (!impl_->beginCommands(error)) return false;

    VkCommandBuffer commandBuffer = impl_->commandBuffer;
    vkCmdFillBuffer(commandBuffer, impl_->statsBuffer.buffer, 0,
                    impl_->statsBuffer.size, 0);
    VkMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clearBarrier,
        0, nullptr, 0, nullptr);

    const PreviewPush push{
        request.width, request.height, request.edgeOverlay ? 1u : 0u, 0u,
        request.orbitYaw, request.orbitPitch, request.zoom, 0.0f
    };
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      family == ShapeFamily::Fracture
                          ? impl_->fracturePreviewPipeline : impl_->previewPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        impl_->pipelineLayout, 0, 1, &impl_->descriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, impl_->pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (request.width + 15u) / 16u,
                  (request.height + 15u) / 16u, 1);

    VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier,
        0, nullptr, 0, nullptr);
    double gpuMilliseconds = 0.0;
    if (!impl_->submitCommands(gpuMilliseconds, error)) return false;

    output.width = request.width;
    output.height = request.height;
    output.gpuName = impl_->selectedGpu;
    output.renderMilliseconds = gpuMilliseconds;
    output.rgba8.resize(pixelCount);
    std::memcpy(output.rgba8.data(), impl_->resolved.mapped,
                static_cast<std::size_t>(pixelCount) * sizeof(std::uint32_t));
    const auto* gpuStats = static_cast<const std::uint32_t*>(impl_->statsBuffer.mapped);
    output.nonBackgroundPixels = std::min<std::uint64_t>(pixelCount, gpuStats[6]);
    stats = {};
    stats.resolvedFamily = family;
    stats.shadedPixels = gpuStats[6];
    stats.endToEndMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - callStart).count();
    return true;
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
    const bool saved = SDL_SavePNG(surface, path.string().c_str());
    if (!saved) error = std::string("Could not save PNG: ") + SDL_GetError();
    SDL_DestroySurface(surface);
    return saved;
}

} // namespace glasslight
