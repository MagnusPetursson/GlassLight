#include "core/composition.hpp"
#include "core/persistence.hpp"
#include "media/png_metadata.hpp"
#include "media/video_export.hpp"
#include "render/vulkan_renderer.hpp"
#include "studio/studio_ui.hpp"

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef GLASSLIGHT_VERSION
#define GLASSLIGHT_VERSION "0.2.0"
#endif

namespace {

constexpr std::uint32_t kPreviewWidth = 1280;
constexpr std::uint32_t kPreviewHeight = 720;

std::string preferredGpuName() {
    const char* value = std::getenv("GLASSLIGHT_GPU");
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

bool uploadTexture(SDL_Renderer* renderer, SDL_Texture*& texture,
                   const glasslight::RenderedImage& image, std::string& error) {
    if (texture) {
        float existingWidth = 0.0f;
        float existingHeight = 0.0f;
        if (SDL_GetTextureSize(texture, &existingWidth, &existingHeight) &&
            static_cast<std::uint32_t>(existingWidth) == image.width &&
            static_cast<std::uint32_t>(existingHeight) == image.height) {
            if (SDL_UpdateTexture(texture, nullptr, image.rgba8.data(),
                                  static_cast<int>(image.width * sizeof(std::uint32_t)))) {
                return true;
            }
            error = "Could not update preview texture: " + std::string(SDL_GetError());
            return false;
        }
    }
    SDL_Texture* replacement = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(image.width), static_cast<int>(image.height));
    if (!replacement) {
        error = "Could not create preview texture: " + std::string(SDL_GetError());
        return false;
    }
    if (!SDL_UpdateTexture(replacement, nullptr, image.rgba8.data(),
                           static_cast<int>(image.width * sizeof(std::uint32_t)))) {
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
        state.selection = std::filesystem::path(fileList[0]);
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

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
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
        frame.diagnostics.renderMilliseconds = preview.renderMilliseconds;
        frame.diagnostics.accumulatedPhotons = renderStats.tracedSamples;
        frame.diagnostics.hitRate = static_cast<float>(renderStats.estimatedHitRate);
        frame.diagnostics.accumulationProgress = artworkTexture ? 1.0f : 0.0f;
        frame.diagnostics.exportInProgress = videoJob.active.load();
        const std::uint32_t exportTotal = videoJob.total.load();
        frame.diagnostics.exportProgress = exportTotal == 0 ? 0.0f :
            static_cast<float>(videoJob.completed.load()) / exportTotal;

        const bool wasPlaying = studio.runtime().playing;
        const bool wallWasPaused = studio.wallRenderingPaused();
        const glasslight::StudioActions actions = studio.draw(frame);
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
                                glassPreviewPending = true;
                                status = "Restored " + result->selection->filename().string();
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
                        status = "Saved " + destination.filename().string();
                        warning.clear();
                    } else {
                        warning = error;
                        status = "Still export failed";
                    }
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
                            warning = "FFmpeg was not found. Install ffmpeg to export MP4.";
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
                                {
                                    std::scoped_lock lock(videoJob.resultMutex);
                                    videoJob.succeeded = succeeded;
                                    videoJob.message = succeeded
                                        ? "Saved " + destination.filename().string()
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

        if (renderPending && gpuReady && !videoJob.active.load() &&
            !studio.wallRenderingPaused()) {
            std::string error;
            if (renderComposition(artRenderer, studio.settings(), kPreviewWidth,
                                  kPreviewHeight, 1.0, preview,
                                  renderStats, error)) {
                if (uploadTexture(presentationRenderer, artworkTexture, preview, error)) {
                    status = studio.runtime().playing ? "Live — one-turn seamless loop" : "Paused";
                    warning.clear();
                } else {
                    warning = error;
                }
            } else {
                warning = error;
                status = "Render failed";
            }
            renderPending = false;
        }

        const bool glassPreviewVisible = studio.runtime().uiVisible &&
            (studio.runtime().glassPreviewExpanded ||
             studio.runtime().glassPreviewLightbox);
        const auto currentTime = std::chrono::steady_clock::now();
        if (glassPreviewPending && glassPreviewVisible && gpuReady &&
            !videoJob.active.load() && currentTime >= nextGlassPreview) {
            glasslight::GlassPreviewRequest request;
            request.settings = studio.settings();
            request.width = studio.runtime().glassPreviewTargetWidth;
            request.height = studio.runtime().glassPreviewTargetHeight;
            request.orbitYaw = 0.55f + studio.runtime().glassPreviewYaw;
            request.orbitPitch = 0.25f + studio.runtime().glassPreviewPitch;
            request.zoom = studio.runtime().glassPreviewZoom;
            request.edgeOverlay = studio.effectiveGlassPreviewEdges();
            std::string error;
            if (artRenderer.renderGlassPreview(
                    request, glassPreview, glassPreviewStats, error)) {
                if (!uploadTexture(presentationRenderer, glassPreviewTexture,
                                   glassPreview, error)) {
                    warning = error;
                }
            } else {
                warning = error;
            }
            glassPreviewPending = false;
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
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::cout << "GlassLight " << GLASSLIGHT_VERSION << '\n';
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--smoke-test") {
        if (argc < 3) {
            std::cerr << "--smoke-test requires an output directory.\n";
            return 2;
        }
        return runSmokeTest(argv[2]);
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
            << "  glasslight --video-smoke FILE\n"
            << "  glasslight --list-gpus | --gpu-info\n"
            << "Set GLASSLIGHT_GPU to an exact listed device name to override Auto.\n";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--video-smoke") {
        return runVideoSmoke(argv[2]);
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
        return renderStillFromCli(argv[2], seed, family);
    }
    return runStudio();
}
