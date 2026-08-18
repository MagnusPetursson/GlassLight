#include "core/composition.hpp"
#include "core/persistence.hpp"
#include "media/png_metadata.hpp"
#include "media/video_export.hpp"
#include "platform/platform.hpp"
#include "render/vulkan_renderer.hpp"
#include "studio/studio_ui.hpp"

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef GLASSLIGHT_VERSION
#define GLASSLIGHT_VERSION "0.2.0"
#endif

namespace {

constexpr std::uint32_t kPreviewWidth = 1280;
constexpr std::uint32_t kPreviewHeight = 720;

void attachParentConsoleForCli() {
#if defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        return;
    }
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

std::string preferredGpuName() {
    const char* value = SDL_GetEnvironmentVariable(
        SDL_GetEnvironment(), "GLASSLIGHT_GPU");
    return value ? std::string(value) : std::string{};
}

std::uint32_t sampleCount(const glasslight::CompositionSettings& settings,
                          const std::uint32_t width,
                          const std::uint32_t height,
                          const double scale = 1.0) {
    const double megapixels = static_cast<double>(width) * height / 1'000'000.0;
    const double requested = std::max(65'536.0,
        megapixels * glasslight::photonsPerMegapixel(settings.quality) * scale);
    return static_cast<std::uint32_t>(std::min(
        requested, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

glasslight::media::RgbaImage asMediaImage(const glasslight::RenderedImage& image) {
    glasslight::media::RgbaImage result;
    result.width = image.width;
    result.height = image.height;
    result.pixels.resize(image.rgba8.size() * sizeof(std::uint32_t));
    if (!result.pixels.empty()) {
        std::memcpy(result.pixels.data(), image.rgba8.data(), result.pixels.size());
    }
    return result;
}

glasslight::RenderedImage asRenderedImage(const glasslight::media::RgbaImage& image) {
    glasslight::RenderedImage result;
    result.width = image.width;
    result.height = image.height;
    result.rgba8.resize(image.pixels.size() / sizeof(std::uint32_t));
    if (!image.pixels.empty()) {
        std::memcpy(result.rgba8.data(), image.pixels.data(), image.pixels.size());
    }
    return result;
}

std::filesystem::path ensureExtension(std::filesystem::path path,
                                      const std::string& extension) {
    if (path.extension().empty()) {
        path += extension;
    }
    return path;
}

bool uploadTexturePixels(SDL_Renderer* renderer, SDL_Texture*& texture,
                         const std::uint32_t* pixels,
                         const std::uint32_t width,
                         const std::uint32_t height,
                         const std::size_t pitchBytes,
                         std::string& error) {
    if (texture) {
        float existingWidth = 0.0f;
        float existingHeight = 0.0f;
        if (SDL_GetTextureSize(texture, &existingWidth, &existingHeight) &&
            static_cast<std::uint32_t>(existingWidth) == width &&
            static_cast<std::uint32_t>(existingHeight) == height) {
            if (SDL_UpdateTexture(texture, nullptr, pixels,
                                  static_cast<int>(pitchBytes))) {
                return true;
            }
            error = "Could not update preview texture: " + std::string(SDL_GetError());
            return false;
        }
    }
    SDL_Texture* replacement = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(width), static_cast<int>(height));
    if (!replacement) {
        error = "Could not create preview texture: " + std::string(SDL_GetError());
        return false;
    }
    if (!SDL_UpdateTexture(replacement, nullptr, pixels,
                           static_cast<int>(pitchBytes))) {
        error = "Could not upload preview texture: " + std::string(SDL_GetError());
        SDL_DestroyTexture(replacement);
        return false;
    }
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    texture = replacement;
    return true;
}

bool uploadTexture(SDL_Renderer* renderer, SDL_Texture*& texture,
                   const glasslight::RenderedImage& image, std::string& error) {
    return uploadTexturePixels(renderer, texture, image.rgba8.data(),
        image.width, image.height, image.width * sizeof(std::uint32_t), error);
}

bool uploadTexture(SDL_Renderer* renderer, SDL_Texture*& texture,
                   const glasslight::ReadbackFrameView& frame, std::string& error) {
    return uploadTexturePixels(renderer, texture, frame.rgba8,
        frame.width, frame.height, frame.pitchBytes, error);
}

void drawFullscreenArtwork(SDL_Renderer* renderer, SDL_Texture* texture,
                           const std::uint32_t width, const std::uint32_t height) {
    if (!texture || width == 0 || height == 0) {
        return;
    }
    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &outputWidth, &outputHeight);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    float drawWidth = static_cast<float>(outputWidth);
    float drawHeight = drawWidth / aspect;
    if (drawHeight > outputHeight) {
        drawHeight = static_cast<float>(outputHeight);
        drawWidth = drawHeight * aspect;
    }
    const SDL_FRect destination{
        (static_cast<float>(outputWidth) - drawWidth) * 0.5f,
        (static_cast<float>(outputHeight) - drawHeight) * 0.5f,
        drawWidth, drawHeight};
    SDL_RenderTexture(renderer, texture, nullptr, &destination);
}

enum class DialogPurpose { None, SaveStill, SaveVideo, RestorePng };

struct DialogState {
    std::mutex mutex;
    DialogPurpose purpose = DialogPurpose::None;
    bool active = false;
    bool ready = false;
    std::optional<std::filesystem::path> selection;
    std::string error;
};

void SDLCALL dialogCallback(void* userdata, const char* const* fileList, int) {
    auto& state = *static_cast<DialogState*>(userdata);
    std::scoped_lock lock(state.mutex);
    state.selection.reset();
    state.error.clear();
    if (!fileList) {
        state.error = SDL_GetError();
    } else if (fileList[0]) {
        state.selection = glasslight::platform::pathFromUtf8(fileList[0]);
    }
    state.active = false;
    state.ready = true;
}

void openDialog(DialogState& state, const DialogPurpose purpose, SDL_Window* window) {
    static const SDL_DialogFileFilter pngFilter[]{{"GlassLight PNG", "png"}};
    static const SDL_DialogFileFilter mp4Filter[]{{"MPEG-4 video", "mp4"}};
    {
        std::scoped_lock lock(state.mutex);
        if (state.active) {
            return;
        }
        state.purpose = purpose;
        state.active = true;
        state.ready = false;
        state.selection.reset();
        state.error.clear();
    }
    if (purpose == DialogPurpose::RestorePng) {
        SDL_ShowOpenFileDialog(dialogCallback, &state, window, pngFilter, 1, nullptr, false);
    } else if (purpose == DialogPurpose::SaveVideo) {
        SDL_ShowSaveFileDialog(dialogCallback, &state, window, mp4Filter, 1,
                               "glasslight-loop.mp4");
    } else {
        SDL_ShowSaveFileDialog(dialogCallback, &state, window, pngFilter, 1,
                               "glasslight.png");
    }
}

struct DialogResult {
    DialogPurpose purpose = DialogPurpose::None;
    std::optional<std::filesystem::path> selection;
    std::string error;
};

std::optional<DialogResult> consumeDialog(DialogState& state) {
    std::scoped_lock lock(state.mutex);
    if (!state.ready) {
        return std::nullopt;
    }
    DialogResult result{state.purpose, state.selection, state.error};
    state.ready = false;
    state.purpose = DialogPurpose::None;
    return result;
}

struct VideoJob {
    std::thread thread;
    std::atomic<bool> active{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::atomic<std::uint32_t> completed{0};
    std::atomic<std::uint32_t> total{0};
    std::mutex resultMutex;
    bool succeeded = false;
    std::string message;

    void join() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

bool renderComposition(glasslight::VulkanRenderer& renderer,
                       const glasslight::CompositionSettings& settings,
                       const std::uint32_t width, const std::uint32_t height,
                       const double qualityScale, glasslight::RenderedImage& image,
                       glasslight::RenderStats& stats, std::string& error,
                       const bool exportRender = false) {
    glasslight::RenderRequest request;
    request.settings = settings;
    request.width = width;
    request.height = height;
    request.samples = sampleCount(settings, width, height, qualityScale);
    request.exportRender = exportRender;
    return renderer.render(request, image, stats, error);
}

int runSmokeTest(const std::filesystem::path& outputDirectory) {
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create smoke-test directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }

    glasslight::VulkanRenderer renderer;
    std::string error;
    if (!renderer.initialize(preferredGpuName(), error)) {
        std::cerr << error << '\n';
        return 2;
    }

    glasslight::CompositionSettings settings;
    settings.seed = 0x474c4153534c4954ULL;
    settings.shapeFamily = glasslight::ShapeFamily::Auto;
    settings.quality = glasslight::QualityPreset::Draft;
    glasslight::RenderedImage image;
    glasslight::RenderStats stats;
    if (!renderComposition(renderer, settings, 640, 360, 1.0, image, stats, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::filesystem::path imagePath = outputDirectory / "glasslight-smoke.png";
    if (!glasslight::media::savePngWithSettings(
            imagePath, asMediaImage(image), glasslight::serializeSettings(settings), error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const bool passed = image.nonBackgroundPixels >
        static_cast<std::uint64_t>(image.width) * image.height / 100u;
    nlohmann::json report = {
        {"version", GLASSLIGHT_VERSION}, {"gpu", image.gpuName},
        {"width", image.width}, {"height", image.height},
        {"samples", stats.tracedSamples}, {"deposits", stats.depositedSamples},
        {"seed", settings.seed}, {"shape", glasslight::shapeFamilyName(stats.resolvedFamily)},
        {"render_ms", image.renderMilliseconds},
        {"non_background_pixels", image.nonBackgroundPixels}, {"passed", passed}};
    std::ofstream(outputDirectory / "report.json") << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return passed ? 0 : 1;
}

int renderStillFromCli(const std::filesystem::path& outputPath,
                       const std::uint64_t seed,
                       const glasslight::ShapeFamily family) {
    glasslight::VulkanRenderer renderer;
    std::string error;
    if (!renderer.initialize(preferredGpuName(), error)) {
        std::cerr << error << '\n';
        return 2;
    }
    glasslight::CompositionSettings settings;
    settings.seed = seed == 0 ? glasslight::materializeSeed() : seed;
    settings.shapeFamily = family;
    settings.quality = glasslight::QualityPreset::Standard;
    glasslight::RenderedImage image;
    glasslight::RenderStats stats;
    if (!renderComposition(renderer, settings, 1920, 1080, 1.0, image, stats, error, true) ||
        !glasslight::media::savePngWithSettings(
            ensureExtension(outputPath, ".png"), asMediaImage(image),
            glasslight::serializeSettings(settings), error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Rendered " << glasslight::shapeFamilyName(stats.resolvedFamily)
              << " seed " << settings.seed << " on " << image.gpuName << " in "
              << image.renderMilliseconds << " ms.\n";
    return 0;
}

std::optional<glasslight::ShapeFamily> parseFamily(const std::string& name) {
    if (name == "auto") return glasslight::ShapeFamily::Auto;
    if (name == "pebble") return glasslight::ShapeFamily::Pebble;
    if (name == "lens") return glasslight::ShapeFamily::Lens;
    if (name == "ribbon") return glasslight::ShapeFamily::Ribbon;
    if (name == "faceted-vessel" || name == "vessel") {
        return glasslight::ShapeFamily::FacetedVessel;
    }
    if (name == "cut-crystal" || name == "crystal") {
        return glasslight::ShapeFamily::CutCrystal;
    }
    if (name == "fracture") return glasslight::ShapeFamily::Fracture;
    return std::nullopt;
}

int runVideoSmoke(const std::filesystem::path& outputPath) {
    glasslight::VulkanRenderer renderer;
    std::string error;
    if (!renderer.initialize(preferredGpuName(), error)) {
        std::cerr << error << '\n';
        return 2;
    }
    const auto ffmpeg = glasslight::media::findFfmpeg();
    if (!ffmpeg) {
        std::cerr << "FFmpeg was not found.\n";
        return 2;
    }
    glasslight::CompositionSettings settings;
    settings.seed = 0x564944454f534d4fULL;
    settings.shapeFamily = glasslight::ShapeFamily::Ribbon;
    settings.quality = glasslight::QualityPreset::Draft;
    glasslight::media::VideoExportConfig config;
    config.outputPath = ensureExtension(outputPath, ".mp4");
    config.ffmpegPath = *ffmpeg;
    config.width = 320;
    config.height = 180;
    config.fps = 30;
    config.frameCount = 2;
    config.settingsJson = glasslight::serializeSettings(settings);
    const bool succeeded = glasslight::media::exportMp4(
        config,
        [&](const std::uint32_t index, std::vector<std::uint8_t>& pixels,
            std::string& frameError) {
            auto frameSettings = settings;
            frameSettings.phase = static_cast<float>(index) / config.frameCount;
            glasslight::RenderedImage frame;
            glasslight::RenderStats stats;
            if (!renderComposition(renderer, frameSettings, config.width, config.height,
                                   1.0, frame, stats, frameError, true)) {
                return false;
            }
            pixels = asMediaImage(frame).pixels;
            return true;
        }, {}, error);
    if (!succeeded) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Wrote two-frame Vulkan/FFmpeg smoke loop to "
              << config.outputPath << '\n';
    return 0;
}

double percentile(std::vector<double> values, const double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(std::ceil(
        fraction * static_cast<double>(values.size()))) - 1u;
    return values[std::min(index, values.size() - 1u)];
}

bool waitForTicket(glasslight::VulkanRenderer& renderer,
                   const glasslight::RenderTicket ticket,
                   glasslight::RenderPollResult& result,
                   std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    do {
        result = renderer.poll(ticket);
        if (result.state == glasslight::RenderPollState::Ready) return true;
        if (result.state == glasslight::RenderPollState::Failed) {
            error = result.error;
            return false;
        }
        if (result.state == glasslight::RenderPollState::Superseded) {
            error = "Benchmark request was unexpectedly superseded.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    error = "Benchmark request timed out after five minutes.";
    return false;
}

nlohmann::json allocationJson(const glasslight::MemoryAllocationDiagnostics& memory) {
    return {{"memory_type", memory.memoryTypeIndex},
            {"device_local", memory.deviceLocal},
            {"host_visible", memory.hostVisible},
            {"host_cached", memory.hostCached},
            {"host_coherent", memory.hostCoherent}};
}

int runBenchmark(const std::filesystem::path& outputDirectory) {
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create benchmark directory: "
                  << filesystemError.message() << '\n';
        return 2;
    }
    struct BenchmarkPresentation {
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        ~BenchmarkPresentation() {
            if (texture) SDL_DestroyTexture(texture);
            if (renderer) SDL_DestroyRenderer(renderer);
            if (window) SDL_DestroyWindow(window);
            SDL_Quit();
        }
    } presentation;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "Could not initialize SDL for benchmark upload timing: "
                  << SDL_GetError() << '\n';
        return 2;
    }
    presentation.window = SDL_CreateWindow(
        "GlassLight benchmark", 64, 64, SDL_WINDOW_HIDDEN);
    presentation.renderer = presentation.window
        ? SDL_CreateRenderer(presentation.window, nullptr) : nullptr;
    if (!presentation.window || !presentation.renderer) {
        std::cerr << "Could not create the SDL benchmark presentation path: "
                  << SDL_GetError() << '\n';
        return 2;
    }

    glasslight::VulkanRenderer renderer;
    std::string error;
    if (!renderer.initialize(preferredGpuName(), error)) {
        std::cerr << error << '\n';
        return 2;
    }

    // Force representative allocations before recording their selected heaps.
    glasslight::CompositionSettings settings;
    settings.seed = 0x42454e43484d4152ULL;
    settings.shapeFamily = glasslight::ShapeFamily::Pebble;
    settings.quality = glasslight::QualityPreset::Standard;
    glasslight::RenderRequest warm;
    warm.settings = settings;
    warm.width = kPreviewWidth;
    warm.height = kPreviewHeight;
    warm.samples = static_cast<std::uint32_t>(std::llround(
        glasslight::photonsPerMegapixel(settings.quality) *
        static_cast<double>(warm.width) * warm.height / 1'000'000.0));
    glasslight::RenderTicket warmTicket = renderer.submit(warm, error);
    glasslight::RenderPollResult warmResult;
    if (!warmTicket || !waitForTicket(renderer, warmTicket, warmResult, error)) {
        if (warmTicket) renderer.release(warmTicket);
        std::cerr << "Benchmark warm-up failed: " << error << '\n';
        return 2;
    }
    renderer.release(warmTicket);

    const auto diagnostics = renderer.diagnostics();
    nlohmann::json system{
        {"version", GLASSLIGHT_VERSION},
        {"os", SDL_GetPlatform()},
        {"gpu", diagnostics.deviceName},
        {"driver", diagnostics.driverName},
        {"driver_version", diagnostics.driverVersion},
        {"timestamp_support", diagnostics.timestampAvailable},
        {"timestamp_period_ns", diagnostics.timestampPeriodNanoseconds},
        {"transientAllocationBytes", diagnostics.transientAllocationBytes},
        {"settingsPayloadBytes", diagnostics.settingsPayloadBytes}};
    system["allocations"] = {
        {"accumulation", allocationJson(diagnostics.accumulation)},
        {"settings", allocationJson(diagnostics.settings)},
        {"upload", allocationJson(diagnostics.upload)},
        {"readback", allocationJson(diagnostics.readback)}};
    system["workgroups"] = {
        {"selected_trace", 256}, {"selected_image", 16},
        {"trace_candidates", {128, 256}}, {"image_candidates", {8, 16}}};
    std::ofstream systemFile(outputDirectory / "system.json", std::ios::trunc);
    std::ofstream report(outputDirectory / "benchmark.tsv", std::ios::trunc);
    if (!systemFile || !report) {
        std::cerr << "Could not create benchmark reports.\n";
        return 2;
    }
    systemFile << system.dump(2) << '\n';
    report << "workload\tmode\twidth\theight\tphotons\tmedian_trace_ms"
              "\tmedian_resolve_ms\tmedian_transfer_ms\tmedian_sdl_upload_ms"
              "\tmedian_e2e_ms"
              "\tp95_e2e_ms\tmax_interval_ms\tdelivered_fps"
              "\tfloor\tceiling\tpass_state\n";

    std::vector<double> traceTimes;
    std::vector<double> resolveTimes;
    std::vector<double> transferTimes;
    std::vector<double> sdlUploadTimes;
    std::vector<double> endToEndTimes;
    const auto record = [&](const glasslight::RenderPollResult& value) {
        traceTimes.push_back(value.timings.traceMilliseconds);
        resolveTimes.push_back(value.timings.resolveMilliseconds);
        transferTimes.push_back(value.timings.transferMilliseconds);
        sdlUploadTimes.push_back(value.timings.sdlUploadMilliseconds);
        endToEndTimes.push_back(value.timings.submitToReadyMilliseconds +
                                value.timings.sdlUploadMilliseconds);
    };
    for (int index = 0; index < 5; ++index) {
        warm.settings.phase = static_cast<float>(index) / 5.0f;
        const auto ticket = renderer.submit(warm, error);
        glasslight::RenderPollResult result;
        if (!ticket || !waitForTicket(renderer, ticket, result, error)) {
            if (ticket) renderer.release(ticket);
            std::cerr << "Fixed benchmark failed: " << error << '\n';
            return 2;
        }
        record(result);
        renderer.release(ticket);
    }
    report << "wall-standard\tfixed\t1280\t720\t" << warm.samples << '\t'
           << percentile(traceTimes, 0.5) << '\t' << percentile(resolveTimes, 0.5) << '\t'
           << percentile(transferTimes, 0.5) << '\t'
           << percentile(sdlUploadTimes, 0.5) << '\t'
           << percentile(endToEndTimes, 0.5) << '\t' << percentile(endToEndTimes, 0.95)
           << "\t0\t0\t" << warm.samples << '\t' << warm.samples << "\tmeasured\n";

    const auto benchmarkWallCandidate = [&](const char* name,
                                            const std::uint32_t traceGroup,
                                            const std::uint32_t imageGroup) {
        traceTimes.clear(); resolveTimes.clear(); transferTimes.clear();
        endToEndTimes.clear(); sdlUploadTimes.clear();
        for (int index = 0; index < 3; ++index) {
            glasslight::RenderRequest candidate = warm;
            candidate.traceGroupSize = traceGroup;
            candidate.imageGroupSize = imageGroup;
            candidate.settings.phase = static_cast<float>(index) / 3.0f;
            const auto ticket = renderer.submit(candidate, error);
            glasslight::RenderPollResult result;
            if (!ticket || !waitForTicket(renderer, ticket, result, error)) {
                if (ticket) renderer.release(ticket);
                return false;
            }
            record(result);
            renderer.release(ticket);
        }
        report << name << "\tworkgroup-candidate\t1280\t720\t" << warm.samples << '\t'
               << percentile(traceTimes, 0.5) << '\t' << percentile(resolveTimes, 0.5)
               << '\t' << percentile(transferTimes, 0.5) << "\t0\t"
               << percentile(endToEndTimes, 0.5) << '\t'
               << percentile(endToEndTimes, 0.95) << "\t0\t0\t"
               << warm.samples << '\t' << warm.samples << "\tmeasured\n";
        return true;
    };
    if (!benchmarkWallCandidate("trace-group-128", 128u, 16u) ||
        !benchmarkWallCandidate("trace-group-256", 256u, 16u) ||
        !benchmarkWallCandidate("resolve-group-8x8", 256u, 8u) ||
        !benchmarkWallCandidate("resolve-group-16x16", 256u, 16u)) {
        std::cerr << "Workgroup candidate benchmark failed: " << error << '\n';
        return 2;
    }
    const auto benchmarkPreviewCandidate = [&](const char* name,
                                               const std::uint32_t imageGroup) {
        traceTimes.clear(); resolveTimes.clear(); transferTimes.clear();
        endToEndTimes.clear(); sdlUploadTimes.clear();
        for (int index = 0; index < 3; ++index) {
            glasslight::GlassPreviewRequest candidate;
            candidate.settings = settings;
            candidate.width = 320u;
            candidate.height = 180u;
            candidate.orbitYaw += static_cast<float>(index) * 0.1f;
            candidate.imageGroupSize = imageGroup;
            const auto ticket = renderer.submitGlassPreview(candidate, error);
            glasslight::RenderPollResult result;
            if (!ticket || !waitForTicket(renderer, ticket, result, error)) {
                if (ticket) renderer.release(ticket);
                return false;
            }
            record(result);
            renderer.release(ticket);
        }
        report << name << "\tworkgroup-candidate\t320\t180\t0\t"
               << percentile(traceTimes, 0.5) << '\t' << percentile(resolveTimes, 0.5)
               << '\t' << percentile(transferTimes, 0.5) << "\t0\t"
               << percentile(endToEndTimes, 0.5) << '\t'
               << percentile(endToEndTimes, 0.95)
               << "\t0\t0\t0\t0\tmeasured\n";
        return true;
    };
    if (!benchmarkPreviewCandidate("preview-group-8x8", 8u) ||
        !benchmarkPreviewCandidate("preview-group-16x16", 16u)) {
        std::cerr << "Preview workgroup candidate benchmark failed: " << error << '\n';
        return 2;
    }

    glasslight::AdaptiveRenderController controller;
    controller.reset(settings.quality, kPreviewWidth, kPreviewHeight, 0.0);
    traceTimes.clear(); resolveTimes.clear(); transferTimes.clear();
    sdlUploadTimes.clear(); endToEndTimes.clear();
    std::vector<double> intervals;
    const auto adaptiveStart = std::chrono::steady_clock::now();
    auto previousCompletion = adaptiveStart;
    int frameIndex = 0;
    while (std::chrono::steady_clock::now() - adaptiveStart < std::chrono::seconds(4)) {
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - adaptiveStart).count();
        const auto workload = controller.workload(elapsed);
        glasslight::RenderRequest request = warm;
        request.settings.phase = std::fmod(static_cast<float>(frameIndex++) / 180.0f, 1.0f);
        request.samples = workload.photons;
        request.interactiveInterfaceDepth = workload.interfaceDepth;
        const auto ticket = renderer.submit(request, error);
        glasslight::RenderPollResult result;
        if (!ticket || !waitForTicket(renderer, ticket, result, error)) {
            if (ticket) renderer.release(ticket);
            std::cerr << "Adaptive benchmark failed: " << error << '\n';
            return 2;
        }
        const auto uploadStart = std::chrono::steady_clock::now();
        if (!uploadTexture(presentation.renderer, presentation.texture,
                           result.frame, error)) {
            renderer.release(ticket);
            std::cerr << "Adaptive SDL upload failed: " << error << '\n';
            return 2;
        }
        result.timings.sdlUploadMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uploadStart).count();
        renderer.recordSdlUpload(ticket, result.timings.sdlUploadMilliseconds);
        const auto completed = std::chrono::steady_clock::now();
        result.timings.completedFrameIntervalMilliseconds =
            std::chrono::duration<double, std::milli>(completed - previousCompletion).count();
        previousCompletion = completed;
        controller.update(result.timings, elapsed);
        if (completed - adaptiveStart >= std::chrono::seconds(2)) {
            record(result);
            intervals.push_back(result.timings.completedFrameIntervalMilliseconds);
        }
        renderer.release(ticket);
    }
    const double measuredSeconds = std::accumulate(intervals.begin(), intervals.end(), 0.0) /
                                   1000.0;
    const double deliveredFps = measuredSeconds > 0.0
        ? static_cast<double>(intervals.size()) / measuredSeconds : 0.0;
    const double medianEndToEnd = percentile(endToEndTimes, 0.5);
    const double p95EndToEnd = percentile(endToEndTimes, 0.95);
    const double maximumStall = intervals.empty() ? 0.0 :
        *std::max_element(intervals.begin(), intervals.end());
    const bool adaptivePassed = deliveredFps >= 28.0 && medianEndToEnd <= 33.3 &&
                                p95EndToEnd <= 40.0 && maximumStall <= 100.0;
    const auto finalWorkload = controller.workload(4'000.0);
    report << "live-rotation\tadaptive\t1280\t720\t" << finalWorkload.photons << '\t'
           << percentile(traceTimes, 0.5) << '\t' << percentile(resolveTimes, 0.5) << '\t'
           << percentile(transferTimes, 0.5) << '\t'
           << percentile(sdlUploadTimes, 0.5) << '\t' << medianEndToEnd << '\t'
           << p95EndToEnd << '\t' << maximumStall << '\t' << deliveredFps
           << "\t460800\t" << warm.samples
           << '\t' << (adaptivePassed ? "pass" : "fail") << '\n';

    struct ExactWorkload { const char* name; std::uint32_t width; std::uint32_t height;
                           glasslight::QualityPreset quality; bool preview; };
    const std::array exactWorkloads{
        ExactWorkload{"card-preview", 320u, 180u, glasslight::QualityPreset::Draft, true},
        ExactWorkload{"lightbox-preview", 768u, 768u, glasslight::QualityPreset::Standard, true},
        ExactWorkload{"video-frame", 1920u, 1080u, glasslight::QualityPreset::Standard, false},
        ExactWorkload{"still-4k", 3840u, 2160u, glasslight::QualityPreset::Standard, false}};
    for (const auto& exact : exactWorkloads) {
        glasslight::RenderPollResult result;
        std::uint32_t photons = 0;
        glasslight::RenderTicket ticket;
        if (exact.preview) {
            glasslight::GlassPreviewRequest request;
            request.settings = settings;
            request.width = exact.width;
            request.height = exact.height;
            ticket = renderer.submitGlassPreview(request, error);
        } else {
            glasslight::RenderRequest request;
            request.settings = settings;
            request.settings.quality = exact.quality;
            request.width = exact.width;
            request.height = exact.height;
            photons = static_cast<std::uint32_t>(std::llround(
                glasslight::photonsPerMegapixel(exact.quality) *
                static_cast<double>(exact.width) * exact.height / 1'000'000.0));
            request.samples = photons;
            request.exportRender = true;
            ticket = renderer.submit(request, error);
        }
        if (!ticket || !waitForTicket(renderer, ticket, result, error)) {
            if (ticket) renderer.release(ticket);
            std::cerr << exact.name << " benchmark failed: " << error << '\n';
            return 2;
        }
        report << exact.name << "\tfixed\t" << exact.width << '\t' << exact.height << '\t'
               << photons << '\t' << result.timings.traceMilliseconds << '\t'
               << result.timings.resolveMilliseconds << '\t'
               << result.timings.transferMilliseconds << '\t'
               << result.timings.sdlUploadMilliseconds << '\t'
               << result.timings.submitToReadyMilliseconds << '\t'
               << result.timings.submitToReadyMilliseconds << "\t0\t0\t" << photons << '\t'
               << photons << "\tmeasured\n";
        renderer.release(ticket);
    }

    renderer.trimTransientResources();
    // A compact deterministic contact sheet keeps aesthetic review local to
    // the benchmark package without imposing golden-pixel requirements.
    constexpr std::uint32_t tileWidth = 320u;
    constexpr std::uint32_t tileHeight = 180u;
    glasslight::RenderedImage sheet;
    sheet.width = tileWidth * 3u;
    sheet.height = tileHeight * 2u;
    sheet.rgba8.resize(static_cast<std::size_t>(sheet.width) * sheet.height);
    for (std::uint32_t familyIndex = 0; familyIndex < 6u; ++familyIndex) {
        glasslight::CompositionSettings familySettings = settings;
        familySettings.shapeFamily = static_cast<glasslight::ShapeFamily>(familyIndex + 1u);
        familySettings.seed += familyIndex * 0x10001u;
        glasslight::RenderedImage tile;
        glasslight::RenderStats tileStats;
        if (!renderComposition(renderer, familySettings, tileWidth, tileHeight,
                               1.0, tile, tileStats, error)) {
            std::cerr << "Contact-sheet render failed: " << error << '\n';
            return 2;
        }
        const std::uint32_t offsetX = (familyIndex % 3u) * tileWidth;
        const std::uint32_t offsetY = (familyIndex / 3u) * tileHeight;
        for (std::uint32_t row = 0; row < tileHeight; ++row) {
            std::copy_n(tile.rgba8.begin() + static_cast<std::size_t>(row) * tileWidth,
                        tileWidth,
                        sheet.rgba8.begin() + static_cast<std::size_t>(offsetY + row) *
                            sheet.width + offsetX);
        }
    }
    sheet.nonBackgroundPixels = sheet.rgba8.size();
    if (!glasslight::savePng(sheet, outputDirectory / "contact-sheet.png", error)) {
        std::cerr << error << '\n';
        return 2;
    }
    report.flush();
    std::cout << "Wrote adaptive benchmark reports to " << outputDirectory
              << " on " << renderer.gpuName() << ".\n";
    return adaptivePassed ? 0 : 1;
}

int runStudio() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "Could not initialize SDL: " << SDL_GetError() << '\n';
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "GlassLight", 1440, 960,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::cerr << "Could not create window: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* presentationRenderer = SDL_CreateRenderer(window, nullptr);
    if (!presentationRenderer) {
        std::cerr << "Could not create presentation renderer: " << SDL_GetError() << '\n';
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(presentationRenderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    glasslight::StudioUi::applyTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, presentationRenderer);
    ImGui_ImplSDLRenderer3_Init(presentationRenderer);

    glasslight::CompositionSettings initialSettings;
    bool foundLastSession = false;
    std::string persistenceError;
    if (!glasslight::loadLastSession(initialSettings, foundLastSession, persistenceError)) {
        initialSettings = {};
    }
    if (!foundLastSession || initialSettings.seed == 0) {
        initialSettings.seed = glasslight::materializeSeed();
    }
    glasslight::StudioUi studio(initialSettings);

    glasslight::VulkanRenderer artRenderer;
    std::string renderError;
    const bool gpuReady = artRenderer.initialize(preferredGpuName(), renderError);
    std::string status = gpuReady ? "Live — one-turn seamless loop" : renderError;
    std::string warning = persistenceError;
    glasslight::RenderedImage preview;
    glasslight::RenderStats renderStats;
    glasslight::RenderedImage glassPreview;
    glasslight::GlassPreviewStats glassPreviewStats;
    SDL_Texture* artworkTexture = nullptr;
    SDL_Texture* glassPreviewTexture = nullptr;
    DialogState dialog;
    VideoJob videoJob;
    bool running = true;
    bool renderPending = gpuReady && studio.runtime().generated;
    bool glassPreviewPending = renderPending;
    auto nextGlassPreview = std::chrono::steady_clock::now();
    const auto studioClockStart = std::chrono::steady_clock::now();
    const auto studioMilliseconds = [&] {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - studioClockStart).count();
    };
    glasslight::AdaptiveRenderController adaptiveController;
    adaptiveController.reset(studio.settings().quality,
                             kPreviewWidth, kPreviewHeight, 0.0);
    glasslight::InteractiveWorkload interactiveWorkload =
        adaptiveController.workload(0.0);
    glasslight::RenderTicket artworkTicket;
    glasslight::RenderTicket glassPreviewTicket;
    glasslight::RenderTimings latestTimings;
    std::uint64_t refinementAccumulated = 0;
    float refinementProgress = 0.0f;
    std::uint32_t pendingArtworkSamples = 0;
    bool pendingArtworkRefinement = false;

    while (running) {
        SDL_Event event;
        bool userInteracted = false;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                       event.type == SDL_EVENT_MOUSE_MOTION ||
                       event.type == SDL_EVENT_MOUSE_WHEEL) {
                userInteracted = true;
            }
        }

        const double nowMilliseconds = studioMilliseconds();
        if (userInteracted) adaptiveController.noteInteraction(nowMilliseconds);

        if (artworkTicket) {
            glasslight::RenderPollResult result = artRenderer.poll(artworkTicket);
            if (result.state == glasslight::RenderPollState::Ready) {
                const auto uploadStart = std::chrono::steady_clock::now();
                std::string error;
                const bool uploaded = uploadTexture(
                    presentationRenderer, artworkTexture, result.frame, error);
                const double uploadMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - uploadStart).count();
                artRenderer.recordSdlUpload(artworkTicket, uploadMilliseconds);
                result.timings.sdlUploadMilliseconds = uploadMilliseconds;
                latestTimings = result.timings;
                renderStats = result.stats;
                if (pendingArtworkRefinement) {
                    refinementAccumulated += pendingArtworkSamples;
                    const double ceiling = glasslight::photonsPerMegapixel(
                        studio.settings().quality) *
                        static_cast<double>(kPreviewWidth) * kPreviewHeight / 1'000'000.0;
                    refinementProgress = std::clamp(static_cast<float>(
                        static_cast<double>(refinementAccumulated) /
                        std::max(ceiling, 1.0)), 0.0f, 1.0f);
                } else {
                    refinementAccumulated = 0;
                    refinementProgress = 0.0f;
                }
                preview.width = result.frame.width;
                preview.height = result.frame.height;
                preview.renderMilliseconds = result.timings.traceMilliseconds +
                    result.timings.resolveMilliseconds + result.timings.transferMilliseconds;
                const float priorPreviewScale = interactiveWorkload.glassPreviewScale;
                adaptiveController.update(result.timings, nowMilliseconds);
                interactiveWorkload = adaptiveController.workload(nowMilliseconds);
                if (interactiveWorkload.glassPreviewScale != priorPreviewScale) {
                    glassPreviewPending = true;
                }
                if (uploaded) {
                    status = studio.runtime().playing
                        ? "Live — adaptive 30 fps stream" : "Paused — refining";
                } else {
                    warning = error;
                }
                artRenderer.release(artworkTicket);
                artworkTicket = {};
                if (studio.runtime().generated && !videoJob.active.load() &&
                    !studio.wallRenderingPaused() &&
                    (studio.runtime().playing || refinementProgress < 1.0f)) {
                    renderPending = true;
                }
            } else if (result.state == glasslight::RenderPollState::Superseded) {
                artRenderer.release(artworkTicket);
                artworkTicket = {};
            } else if (result.state == glasslight::RenderPollState::Failed) {
                warning = result.error;
                status = "Render failed";
                artRenderer.release(artworkTicket);
                artworkTicket = {};
            }
        }

        if (glassPreviewTicket) {
            glasslight::RenderPollResult result = artRenderer.poll(glassPreviewTicket);
            if (result.state == glasslight::RenderPollState::Ready) {
                std::string error;
                if (!uploadTexture(presentationRenderer, glassPreviewTexture,
                                   result.frame, error)) {
                    warning = error;
                }
                glassPreview.width = result.frame.width;
                glassPreview.height = result.frame.height;
                glassPreviewStats.resolvedFamily = result.stats.resolvedFamily;
                glassPreviewStats.shadedPixels = result.stats.depositedSamples;
                artRenderer.release(glassPreviewTicket);
                glassPreviewTicket = {};
            } else if (result.state == glasslight::RenderPollState::Superseded) {
                artRenderer.release(glassPreviewTicket);
                glassPreviewTicket = {};
            } else if (result.state == glasslight::RenderPollState::Failed) {
                warning = result.error;
                artRenderer.release(glassPreviewTicket);
                glassPreviewTicket = {};
            }
        }

        if (videoJob.finished.exchange(false)) {
            videoJob.join();
            std::scoped_lock lock(videoJob.resultMutex);
            status = videoJob.message;
            warning = videoJob.succeeded ? std::string{} : videoJob.message;
            renderPending = studio.runtime().generated;
            glassPreviewPending = studio.runtime().generated;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        glasslight::StudioFrame frame;
        if (artworkTexture) {
            frame.canvasTexture = static_cast<ImTextureID>(
                reinterpret_cast<std::uintptr_t>(artworkTexture));
            frame.canvasWidth = preview.width;
            frame.canvasHeight = preview.height;
        }
        if (glassPreviewTexture) {
            frame.glassPreviewTexture = static_cast<ImTextureID>(
                reinterpret_cast<std::uintptr_t>(glassPreviewTexture));
            frame.glassPreviewWidth = glassPreview.width;
            frame.glassPreviewHeight = glassPreview.height;
        }
        frame.glassPreviewFamily = glassPreviewTexture
            ? glassPreviewStats.resolvedFamily
            : glasslight::resolveShapeFamily(studio.settings());
        frame.diagnostics.gpuName = gpuReady ? artRenderer.gpuName() : "No Vulkan device";
        frame.diagnostics.statusMessage = status;
        frame.diagnostics.warningMessage = warning;
        frame.diagnostics.performanceMessage = interactiveWorkload.performanceNotice
            ? "Interactive rendering is at its minimum workload and remains below the "
              "30 fps target. Exports are unaffected."
            : std::string{};
        frame.diagnostics.gpuStageMilliseconds = latestTimings.traceMilliseconds +
            latestTimings.resolveMilliseconds + latestTimings.transferMilliseconds;
        frame.diagnostics.endToEndMilliseconds = latestTimings.submitToReadyMilliseconds +
            latestTimings.sdlUploadMilliseconds;
        frame.diagnostics.deliveredFps = interactiveWorkload.deliveredFps;
        frame.diagnostics.p95FrameMilliseconds = interactiveWorkload.p95FrameMilliseconds;
        frame.diagnostics.accumulatedPhotons = interactiveWorkload.photons;
        frame.diagnostics.qualityCeiling =
            glasslight::qualityPresetName(studio.settings().quality);
        frame.diagnostics.hitRate = static_cast<float>(renderStats.estimatedHitRate);
        frame.diagnostics.accumulationProgress = refinementProgress;
        frame.diagnostics.exportInProgress = videoJob.active.load();
        const std::uint32_t exportTotal = videoJob.total.load();
        frame.diagnostics.exportProgress = exportTotal == 0 ? 0.0f :
            static_cast<float>(videoJob.completed.load()) / exportTotal;

        const bool wasPlaying = studio.runtime().playing;
        const bool wallWasPaused = studio.wallRenderingPaused();
        const glasslight::StudioActions actions = studio.draw(frame);
        if ((actions.generateRequested ||
             (actions.compositionChanged && userInteracted)) &&
            studio.runtime().generated) {
            adaptiveController.reset(studio.settings().quality,
                                     kPreviewWidth, kPreviewHeight,
                                     nowMilliseconds);
            interactiveWorkload = adaptiveController.workload(nowMilliseconds);
            refinementAccumulated = 0;
            refinementProgress = 0.0f;
        }
        if (actions.fullscreenToggleRequested) {
            SDL_SetWindowFullscreen(window, studio.runtime().fullscreen);
        }
        if (actions.cancelExportRequested) {
            videoJob.cancel.store(true);
        }
        if ((actions.generateRequested || actions.compositionChanged) &&
            studio.runtime().generated && !videoJob.active.load() &&
            !studio.wallRenderingPaused()) {
            renderPending = true;
            if (actions.resetAccumulation) {
                refinementAccumulated = 0;
                refinementProgress = 0.0f;
            }
        }
        if ((actions.generateRequested || actions.compositionChanged ||
             actions.glassPreviewChanged) && studio.runtime().generated) {
            glassPreviewPending = true;
        }
        if (wallWasPaused && !studio.wallRenderingPaused() &&
            studio.runtime().generated) {
            renderPending = true;
        }
        if (wasPlaying != studio.runtime().playing && studio.runtime().generated) {
            adaptiveController.setPaused(!studio.runtime().playing, nowMilliseconds);
            interactiveWorkload = adaptiveController.workload(nowMilliseconds);
            refinementAccumulated = 0;
            refinementProgress = 0.0f;
            renderPending = true;
        }
        if (actions.persistRequested || actions.generateRequested) {
            std::string saveError;
            if (!glasslight::saveLastSession(studio.settings(), saveError)) {
                warning = saveError;
            }
        }
        if (actions.exportStillRequested && !videoJob.active.load()) {
            openDialog(dialog, DialogPurpose::SaveStill, window);
        }
        if (actions.exportVideoRequested && !videoJob.active.load()) {
            openDialog(dialog, DialogPurpose::SaveVideo, window);
        }
        if (actions.restoreFromPngRequested && !videoJob.active.load()) {
            openDialog(dialog, DialogPurpose::RestorePng, window);
        }

        if (const auto result = consumeDialog(dialog)) {
            if (!result->error.empty()) {
                warning = "File dialog failed: " + result->error;
            } else if (result->selection) {
                if (result->purpose == DialogPurpose::RestorePng) {
                    glasslight::media::RgbaImage loadedImage;
                    std::string settingsJson;
                    std::string error;
                    if (!glasslight::media::loadPngWithSettings(
                            *result->selection, loadedImage, settingsJson, error)) {
                        warning = error;
                    } else if (settingsJson.empty()) {
                        warning = "That PNG has no GlassLight composition metadata.";
                    } else {
                        glasslight::CompositionSettings restored;
                        if (!glasslight::deserializeSettings(settingsJson, restored, error)) {
                            warning = error;
                        } else {
                            preview = asRenderedImage(loadedImage);
                            if (uploadTexture(presentationRenderer, artworkTexture,
                                              preview, error)) {
                                studio.setSettings(restored, true);
                                studio.mutableRuntime().playing = false;
                                adaptiveController.reset(restored.quality,
                                    kPreviewWidth, kPreviewHeight, nowMilliseconds);
                                adaptiveController.setPaused(true, nowMilliseconds);
                                interactiveWorkload = adaptiveController.workload(nowMilliseconds);
                                refinementAccumulated = 0;
                                refinementProgress = 0.0f;
                                glassPreviewPending = true;
                                status = "Restored " + glasslight::platform::pathToUtf8(
                                    result->selection->filename());
                                warning.clear();
                                glasslight::saveLastSession(studio.settings(), error);
                            } else {
                                warning = error;
                            }
                        }
                    }
                } else if (result->purpose == DialogPurpose::SaveStill) {
                    const std::filesystem::path destination =
                        ensureExtension(*result->selection, ".png");
                    glasslight::RenderedImage still;
                    glasslight::RenderStats stillStats;
                    std::string error;
                    status = "Rendering still image...";
                    const auto settings = studio.settings();
                    if (renderComposition(artRenderer, settings, settings.stillWidth,
                                          settings.stillHeight, 1.0, still,
                                          stillStats, error, true) &&
                        glasslight::media::savePngWithSettings(
                            destination, asMediaImage(still),
                            glasslight::serializeSettings(settings), error)) {
                        status = "Saved " + glasslight::platform::pathToUtf8(
                            destination.filename());
                        warning.clear();
                    } else {
                        warning = error;
                        status = "Still export failed";
                    }
                    artRenderer.trimTransientResources();
                } else if (result->purpose == DialogPurpose::SaveVideo) {
                    const std::filesystem::path destination =
                        ensureExtension(*result->selection, ".mp4");
                    const glasslight::CompositionSettings settings = studio.settings();
                    const double duration = glasslight::loopDurationSeconds(settings);
                    if (!std::isfinite(duration)) {
                        warning = "Set a non-zero rotation speed before exporting a loop.";
                    } else {
                        const auto ffmpeg = glasslight::media::findFfmpeg();
                        if (!ffmpeg) {
                            warning = "FFmpeg was not found beside GlassLight or on PATH. "
                                      "Add ffmpeg.exe beside the app or install FFmpeg.";
                        } else {
                            videoJob.join();
                            videoJob.cancel.store(false);
                            videoJob.completed.store(0);
                            const std::uint32_t frameCount = static_cast<std::uint32_t>(
                                std::max(1.0, std::round(duration * settings.videoFps)));
                            videoJob.total.store(frameCount);
                            videoJob.active.store(true);
                            status = "Rendering seamless loop...";
                            warning.clear();
                            videoJob.thread = std::thread([
                                &artRenderer, &videoJob, settings, destination,
                                ffmpegPath = *ffmpeg, frameCount] {
                                glasslight::media::VideoExportConfig config;
                                config.outputPath = destination;
                                config.ffmpegPath = ffmpegPath;
                                config.width = (settings.videoWidth + 1u) & ~1u;
                                config.height = (settings.videoHeight + 1u) & ~1u;
                                config.fps = settings.videoFps;
                                config.frameCount = frameCount;
                                config.settingsJson = glasslight::serializeSettings(settings);
                                const float direction = settings.rotationRpm < 0.0f ? -1.0f : 1.0f;
                                std::string exportError;
                                const bool succeeded = glasslight::media::exportMp4(
                                    config,
                                    [&](const std::uint32_t index,
                                        std::vector<std::uint8_t>& pixels,
                                        std::string& error) {
                                        glasslight::CompositionSettings frameSettings = settings;
                                        frameSettings.phase = std::fmod(
                                            settings.phase + direction *
                                                static_cast<float>(index) / frameCount + 1.0f,
                                            1.0f);
                                        glasslight::RenderedImage rendered;
                                        glasslight::RenderStats stats;
                                        if (!renderComposition(
                                                artRenderer, frameSettings, config.width,
                                                config.height, 1.0, rendered, stats,
                                                error, true)) {
                                            return false;
                                        }
                                        pixels = asMediaImage(rendered).pixels;
                                        return true;
                                    },
                                    {{[&](const glasslight::media::VideoExportProgress& progress) {
                                          videoJob.completed.store(progress.completedFrames);
                                      }},
                                     {[&] { return videoJob.cancel.load(); }}},
                                    exportError);
                                artRenderer.trimTransientResources();
                                {
                                    std::scoped_lock lock(videoJob.resultMutex);
                                    videoJob.succeeded = succeeded;
                                    videoJob.message = succeeded
                                        ? "Saved " + glasslight::platform::pathToUtf8(
                                            destination.filename())
                                        : exportError;
                                }
                                videoJob.active.store(false);
                                videoJob.finished.store(true);
                            });
                        }
                    }
                }
            }
        }

        if (renderPending && !artworkTicket && gpuReady &&
            !videoJob.active.load() && !studio.wallRenderingPaused()) {
            interactiveWorkload = adaptiveController.workload(nowMilliseconds);
            glasslight::RenderRequest request;
            request.settings = studio.settings();
            request.width = kPreviewWidth;
            request.height = kPreviewHeight;
            const bool accumulateRefinement = interactiveWorkload.refining &&
                (!studio.runtime().playing ||
                 std::abs(studio.settings().rotationRpm) < 1.0e-6f);
            const std::uint64_t ceilingSamples = static_cast<std::uint64_t>(std::llround(
                glasslight::photonsPerMegapixel(studio.settings().quality) *
                static_cast<double>(kPreviewWidth) * kPreviewHeight / 1'000'000.0));
            const std::uint64_t remaining = ceilingSamples > refinementAccumulated
                ? ceilingSamples - refinementAccumulated : 0u;
            if (accumulateRefinement && remaining == 0u) {
                renderPending = false;
            } else {
                request.samples = accumulateRefinement
                    ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        remaining, interactiveWorkload.photons))
                    : interactiveWorkload.photons;
                request.resolveSamples = accumulateRefinement
                    ? static_cast<std::uint32_t>(refinementAccumulated + request.samples)
                    : request.samples;
                request.sampleOffset = accumulateRefinement ? refinementAccumulated : 0u;
                request.progressivePass = accumulateRefinement
                    ? static_cast<std::uint32_t>(refinementAccumulated /
                        std::max(interactiveWorkload.photons, 1u)) : 0u;
                request.preserveAccumulation = accumulateRefinement &&
                                               refinementAccumulated != 0u;
                request.interactiveInterfaceDepth = interactiveWorkload.interfaceDepth;
                request.slotAffinity = 0u;
                std::string error;
                artworkTicket = artRenderer.submit(request, error);
                if (artworkTicket) {
                    pendingArtworkSamples = request.samples;
                    pendingArtworkRefinement = accumulateRefinement;
                    renderPending = false;
                } else if (error != "All asynchronous render slots are busy.") {
                    warning = error;
                    status = "Render failed";
                    renderPending = false;
                }
            }
        }

        const bool glassPreviewVisible = studio.runtime().uiVisible &&
            (studio.runtime().glassPreviewExpanded ||
             studio.runtime().glassPreviewLightbox);
        const auto currentTime = std::chrono::steady_clock::now();
        if (glassPreviewPending && !glassPreviewTicket && glassPreviewVisible && gpuReady &&
            !videoJob.active.load() && currentTime >= nextGlassPreview) {
            glasslight::GlassPreviewRequest request;
            request.settings = studio.settings();
            const float scale = interactiveWorkload.glassPreviewScale;
            request.width = std::max(1u, static_cast<std::uint32_t>(std::lround(
                studio.runtime().glassPreviewTargetWidth * scale)));
            request.height = std::max(1u, static_cast<std::uint32_t>(std::lround(
                studio.runtime().glassPreviewTargetHeight * scale)));
            request.orbitYaw = 0.55f + studio.runtime().glassPreviewYaw;
            request.orbitPitch = 0.25f + studio.runtime().glassPreviewPitch;
            request.zoom = studio.runtime().glassPreviewZoom;
            request.edgeOverlay = studio.effectiveGlassPreviewEdges() && scale >= 1.0f;
            request.interactive = !interactiveWorkload.refining;
            std::string error;
            glassPreviewTicket = artRenderer.submitGlassPreview(request, error);
            if (glassPreviewTicket) {
                glassPreviewPending = false;
            } else if (error != "All asynchronous render slots are busy.") {
                warning = error;
                glassPreviewPending = false;
            }
            const auto interval = studio.runtime().glassPreviewLightbox ||
                                  studio.runtime().glassPreviewOrbiting
                ? std::chrono::milliseconds(42)
                : std::chrono::milliseconds(90);
            nextGlassPreview = std::chrono::steady_clock::now() + interval;
        }

        SDL_SetRenderDrawColor(presentationRenderer, 8, 12, 15, 255);
        SDL_RenderClear(presentationRenderer);
        if (!studio.runtime().uiVisible) {
            drawFullscreenArtwork(presentationRenderer, artworkTexture,
                                  preview.width, preview.height);
        }
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), presentationRenderer);
        SDL_RenderPresent(presentationRenderer);
    }

    videoJob.cancel.store(true);
    videoJob.join();
    std::string ignored;
    glasslight::saveLastSession(studio.settings(), ignored);
    if (artworkTexture) SDL_DestroyTexture(artworkTexture);
    if (glassPreviewTexture) SDL_DestroyTexture(glassPreviewTexture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(presentationRenderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        attachParentConsoleForCli();
    }
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::cout << "GlassLight " << GLASSLIGHT_VERSION << '\n';
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--smoke-test") {
        if (argc < 3) {
            std::cerr << "--smoke-test requires an output directory.\n";
            return 2;
        }
        return runSmokeTest(glasslight::platform::pathFromUtf8(argv[2]));
    }
    if (argc >= 2 && std::string(argv[1]) == "--benchmark") {
        if (argc != 3) {
            std::cerr << "--benchmark requires exactly one output directory.\n";
            return 2;
        }
        return runBenchmark(glasslight::platform::pathFromUtf8(argv[2]));
    }
    if (argc >= 2 && std::string(argv[1]) == "--gpu-info") {
        glasslight::VulkanRenderer renderer;
        std::string error;
        if (!renderer.initialize(preferredGpuName(), error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << renderer.gpuName() << '\n';
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--list-gpus") {
        std::string error;
        const auto names = glasslight::VulkanRenderer::compatibleGpuNames(error);
        if (names.empty() && !error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        for (const std::string& name : names) {
            std::cout << name << '\n';
        }
        return names.empty() ? 1 : 0;
    }
    if (argc >= 2 && (std::string(argv[1]) == "--help" ||
                      std::string(argv[1]) == "-h")) {
        std::cout
            << "GlassLight " << GLASSLIGHT_VERSION << "\n"
            << "  glasslight                         Open the native studio\n"
            << "  glasslight --render-still FILE [SEED] SHAPE\n"
            << "    SHAPE: auto, pebble, lens, ribbon, faceted-vessel, cut-crystal, fracture\n"
            << "  glasslight --smoke-test DIRECTORY\n"
            << "  glasslight --benchmark DIRECTORY\n"
            << "  glasslight --video-smoke FILE\n"
            << "  glasslight --list-gpus | --gpu-info\n"
            << "Set GLASSLIGHT_GPU to an exact listed device name to override Auto.\n";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--video-smoke") {
        return runVideoSmoke(glasslight::platform::pathFromUtf8(argv[2]));
    }
    if (argc >= 3 && std::string(argv[1]) == "--render-still") {
        std::uint64_t seed = 0;
        if (argc >= 4) {
            try {
                seed = std::stoull(argv[3], nullptr, 0);
            } catch (const std::exception&) {
                std::cerr << "Seed must be an unsigned integer.\n";
                return 2;
            }
        }
        glasslight::ShapeFamily family = glasslight::ShapeFamily::Auto;
        if (argc >= 5) {
            const auto parsed = parseFamily(argv[4]);
            if (!parsed) {
                std::cerr << "Shape must be auto, pebble, lens, ribbon, "
                             "faceted-vessel, cut-crystal, or fracture.\n";
                return 2;
            }
            family = *parsed;
        }
        return renderStillFromCli(
            glasslight::platform::pathFromUtf8(argv[2]), seed, family);
    }
    return runStudio();
}
