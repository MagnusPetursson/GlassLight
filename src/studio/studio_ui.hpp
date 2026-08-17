#pragma once

#include "core/composition.hpp"

#include <imgui.h>

#include <cstdint>
#include <string>

namespace glasslight {

struct StudioDiagnostics {
    std::string gpuName;
    std::string statusMessage;
    std::string warningMessage;
    double renderMilliseconds = 0.0;
    std::uint64_t accumulatedPhotons = 0;
    float accumulationProgress = 0.0f;
    float hitRate = 0.0f;
    bool exportInProgress = false;
    float exportProgress = 0.0f;
};

struct StudioFrame {
    // Pass the backend-native texture identifier after converting it to
    // ImTextureID. Zero displays the themed empty/working canvas instead.
    ImTextureID canvasTexture = ImTextureID_Invalid;
    std::uint32_t canvasWidth = 0;
    std::uint32_t canvasHeight = 0;
    // A distinct host-owned texture. It is shown only in the studio chrome and
    // is never composited into canvas or export output.
    ImTextureID glassPreviewTexture = ImTextureID_Invalid;
    std::uint32_t glassPreviewWidth = 0;
    std::uint32_t glassPreviewHeight = 0;
    ShapeFamily glassPreviewFamily = ShapeFamily::Pebble;
    StudioDiagnostics diagnostics{};
};

struct StudioActions {
    bool compositionChanged = false;
    bool resetAccumulation = false;
    bool generateRequested = false;
    bool exportStillRequested = false;
    bool exportVideoRequested = false;
    bool restoreFromPngRequested = false;
    bool cancelExportRequested = false;
    bool fullscreenToggleRequested = false;
    bool uiVisibilityChanged = false;
    bool persistRequested = false;
    bool glassPreviewChanged = false;
    bool glassPreviewVisibilityChanged = false;
    bool lightboxChanged = false;
};

enum class PreviewEdgeMode : std::uint8_t { Automatic, Off, On };

struct StudioRuntimeState {
    bool generated = false;
    bool playing = true;
    bool uiVisible = true;
    bool fullscreen = false;
    bool showAdvanced = false;
    // Glass preview state deliberately lives outside CompositionSettings. It is
    // session-only and must not be serialized into PNG/MP4 or last-session JSON.
    bool glassPreviewExpanded = true;
    bool glassPreviewLightbox = false;
    PreviewEdgeMode glassPreviewEdges = PreviewEdgeMode::Automatic;
    float glassPreviewYaw = 0.0f;
    float glassPreviewPitch = 0.0f;
    float glassPreviewZoom = 1.0f;
    bool glassPreviewReturningToSync = false;
    bool glassPreviewOrbiting = false;
    std::uint32_t glassPreviewTargetWidth = 256;
    std::uint32_t glassPreviewTargetHeight = 256;
};

// Owns composition and presentation state only. The host owns SDL events,
// textures, rendering, dialogs, exports, and persistence side effects.
class StudioUi {
public:
    explicit StudioUi(CompositionSettings settings = {});

    StudioActions draw(const StudioFrame& frame);

    [[nodiscard]] const CompositionSettings& settings() const;
    CompositionSettings& mutableSettings();
    void setSettings(const CompositionSettings& settings, bool markGenerated = true);
    // Restores composition controls to their striking defaults while retaining
    // the current seed and presentation-only session state.
    void resetOptions();

    [[nodiscard]] const StudioRuntimeState& runtime() const;
    StudioRuntimeState& mutableRuntime();
    [[nodiscard]] bool effectiveGlassPreviewEdges() const;
    [[nodiscard]] bool wallRenderingPaused() const;

    // May be called again after recreating the ImGui context.
    static void applyTheme();

private:
    CompositionSettings settings_{};
    StudioRuntimeState runtime_{};
    std::string validationMessage_;

    void drawCanvas(const StudioFrame& frame, StudioActions& actions);
    void drawInspector(const StudioFrame& frame, StudioActions& actions);
    void drawToolbar(const StudioFrame& frame, StudioActions& actions);
    void drawGlassPreviewCard(const StudioFrame& frame, StudioActions& actions);
    void drawGlassPreviewLightbox(const StudioFrame& frame, StudioActions& actions);
    void drawInteractivePreview(const StudioFrame& frame, const ImVec2& size,
                                bool returnToSyncOnRelease, bool allowWheelZoom,
                                StudioActions& actions);
    void commitEdit(bool edited, StudioActions& actions);
};

} // namespace glasslight
