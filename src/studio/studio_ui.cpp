#include "studio/studio_ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace glasslight {
namespace {

constexpr float kInspectorWidth = 372.0f;

struct NamedPalette {
    const char* name;
    std::array<Color3, 5> colors;
};

constexpr std::array<NamedPalette, 5> kPalettes{{
    {"Cathedral Dawn", {{{0.20f, 0.36f, 0.72f}, {0.20f, 0.67f, 0.56f},
        {0.95f, 0.58f, 0.22f}, {0.78f, 0.28f, 0.38f}, {0.48f, 0.30f, 0.68f}}}},
    {"Northern Veil", {{{0.12f, 0.48f, 0.58f}, {0.18f, 0.78f, 0.61f},
        {0.53f, 0.88f, 0.74f}, {0.41f, 0.46f, 0.91f}, {0.73f, 0.42f, 0.90f}}}},
    {"Ember Chapel", {{{0.48f, 0.08f, 0.05f}, {0.82f, 0.20f, 0.08f},
        {1.00f, 0.49f, 0.12f}, {0.97f, 0.76f, 0.27f}, {0.47f, 0.16f, 0.30f}}}},
    {"Tidal Prism", {{{0.03f, 0.20f, 0.34f}, {0.03f, 0.47f, 0.58f},
        {0.16f, 0.75f, 0.72f}, {0.62f, 0.91f, 0.73f}, {0.26f, 0.46f, 0.76f}}}},
    {"Rose Quartz", {{{0.62f, 0.23f, 0.38f}, {0.91f, 0.42f, 0.52f},
        {0.97f, 0.66f, 0.65f}, {0.83f, 0.63f, 0.82f}, {0.46f, 0.31f, 0.58f}}}}
}};

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool shapeCombo(ShapeFamily& value) {
    bool changed = false;
    if (ImGui::BeginCombo("Shape", shapeFamilyName(value))) {
        for (std::uint32_t raw = 0; raw <= 6; ++raw) {
            const auto candidate = static_cast<ShapeFamily>(raw);
            if (ImGui::Selectable(shapeFamilyName(candidate), candidate == value)) {
                value = candidate;
                changed = true;
            }
            if (candidate == value) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool wallCombo(WallPreset& value) {
    bool changed = false;
    if (ImGui::BeginCombo("Wall", wallPresetName(value))) {
        for (std::uint32_t raw = 0; raw <= 3; ++raw) {
            const auto candidate = static_cast<WallPreset>(raw);
            if (ImGui::Selectable(wallPresetName(candidate), candidate == value)) {
                value = candidate;
                changed = true;
            }
            if (candidate == value) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool qualityCombo(QualityPreset& value) {
    bool changed = false;
    if (ImGui::BeginCombo("Quality", qualityPresetName(value))) {
        for (std::uint32_t raw = 0; raw <= 2; ++raw) {
            const auto candidate = static_cast<QualityPreset>(raw);
            if (ImGui::Selectable(qualityPresetName(candidate), candidate == value)) {
                value = candidate;
                changed = true;
            }
            if (candidate == value) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool fractureMorphologyCombo(FractureMorphology& value) {
    bool changed = false;
    if (ImGui::BeginCombo("Morphology", fractureMorphologyName(value))) {
        for (std::uint32_t raw = 0; raw <= 3; ++raw) {
            const auto candidate = static_cast<FractureMorphology>(raw);
            if (ImGui::Selectable(fractureMorphologyName(candidate), candidate == value)) {
                value = candidate;
                changed = true;
            }
            if (candidate == value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool fractureMotionCombo(FractureMotion& value) {
    bool changed = false;
    if (ImGui::BeginCombo("Motion", fractureMotionName(value))) {
        for (std::uint32_t raw = 0; raw <= 3; ++raw) {
            const auto candidate = static_cast<FractureMotion>(raw);
            if (ImGui::Selectable(fractureMotionName(candidate), candidate == value)) {
                value = candidate;
                changed = true;
            }
            if (candidate == value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void dimensionInput(const char* id, std::uint32_t& value, bool& changed) {
    int editable = static_cast<int>(value);
    ImGui::SetNextItemWidth(94.0f);
    if (ImGui::InputInt(id, &editable, 0, 0)) {
        value = static_cast<std::uint32_t>(std::max(editable, 1));
        changed = true;
    }
}

ImVec2 fitInside(const ImVec2 available, const std::uint32_t width,
                 const std::uint32_t height) {
    if (width == 0 || height == 0 || available.x <= 0.0f || available.y <= 0.0f) {
        return {0.0f, 0.0f};
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    ImVec2 result{available.x, available.x / aspect};
    if (result.y > available.y) {
        result = {available.y * aspect, available.y};
    }
    return result;
}

void centerText(const char* text) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                 ImGui::GetCursorPosX() +
                                 (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    ImGui::TextUnformatted(text);
}

} // namespace

StudioUi::StudioUi(CompositionSettings settings) : settings_(std::move(settings)) {
    const ValidationResult validation = validate(settings_);
    validationMessage_ = validation.warning;
    runtime_.generated = settings_.seed != 0;
}

StudioActions StudioUi::draw(const StudioFrame& frame) {
    StudioActions actions;
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        runtime_.fullscreen = !runtime_.fullscreen;
        actions.fullscreenToggleRequested = true;
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        runtime_.uiVisible = !runtime_.uiVisible;
        actions.uiVisibilityChanged = true;
        if (!runtime_.uiVisible && runtime_.glassPreviewLightbox) {
            runtime_.glassPreviewLightbox = false;
            runtime_.glassPreviewReturningToSync = true;
            runtime_.glassPreviewZoom = 1.0f;
            actions.lightboxChanged = true;
            actions.glassPreviewChanged = true;
        }
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        runtime_.playing = !runtime_.playing;
    }
    if (runtime_.glassPreviewLightbox && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        runtime_.glassPreviewLightbox = false;
        runtime_.glassPreviewReturningToSync = true;
        runtime_.glassPreviewZoom = 1.0f;
        actions.lightboxChanged = true;
        actions.glassPreviewChanged = true;
    }

    if (runtime_.glassPreviewReturningToSync && !runtime_.glassPreviewOrbiting) {
        const float decay = std::exp(-std::max(io.DeltaTime, 0.0f) * 5.5f);
        runtime_.glassPreviewYaw *= decay;
        runtime_.glassPreviewPitch *= decay;
        if (std::abs(runtime_.glassPreviewYaw) < 0.002f &&
            std::abs(runtime_.glassPreviewPitch) < 0.002f) {
            runtime_.glassPreviewYaw = 0.0f;
            runtime_.glassPreviewPitch = 0.0f;
            runtime_.glassPreviewReturningToSync = false;
        }
        actions.glassPreviewChanged = true;
    }

    if (runtime_.generated && runtime_.playing && std::abs(settings_.rotationRpm) > 1.0e-6f) {
        const float previous = settings_.phase;
        settings_.phase += io.DeltaTime * settings_.rotationRpm / 60.0f;
        settings_.phase -= std::floor(settings_.phase);
        if (settings_.phase != previous) {
            actions.compositionChanged = true;
            actions.resetAccumulation = true;
        }
    }

    if (!runtime_.uiVisible) {
        return actions;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("GlassLight Studio", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    drawToolbar(frame, actions);
    const float inspectorWidth = std::min(kInspectorWidth,
                                          ImGui::GetContentRegionAvail().x * 0.48f);
    ImGui::BeginChild("##canvas", ImVec2(-inspectorWidth, 0.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    drawCanvas(frame, actions);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##inspector", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    drawInspector(frame, actions);
    ImGui::EndChild();
    ImGui::End();

    if (runtime_.glassPreviewLightbox) {
        drawGlassPreviewLightbox(frame, actions);
    }

    return actions;
}

const CompositionSettings& StudioUi::settings() const { return settings_; }
CompositionSettings& StudioUi::mutableSettings() { return settings_; }

void StudioUi::setSettings(const CompositionSettings& settings, const bool markGenerated) {
    settings_ = settings;
    const ValidationResult validation = validate(settings_);
    validationMessage_ = validation.warning;
    runtime_.generated = markGenerated && settings_.seed != 0;
}

void StudioUi::resetOptions() {
    const std::uint64_t seed = settings_.seed;
    settings_ = CompositionSettings{};
    settings_.seed = seed;
    const ValidationResult validation = validate(settings_);
    validationMessage_ = validation.warning;
    runtime_.generated = seed != 0;
}

const StudioRuntimeState& StudioUi::runtime() const { return runtime_; }
StudioRuntimeState& StudioUi::mutableRuntime() { return runtime_; }

bool StudioUi::effectiveGlassPreviewEdges() const {
    if (runtime_.glassPreviewEdges == PreviewEdgeMode::On) {
        return true;
    }
    if (runtime_.glassPreviewEdges == PreviewEdgeMode::Off) {
        return false;
    }
    const ShapeFamily family = resolveShapeFamily(settings_);
    return family == ShapeFamily::FacetedVessel || family == ShapeFamily::CutCrystal ||
           family == ShapeFamily::Fracture;
}

bool StudioUi::wallRenderingPaused() const {
    return runtime_.glassPreviewLightbox;
}

void StudioUi::drawToolbar(const StudioFrame& frame, StudioActions& actions) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.035f, 0.045f, 0.98f));
    ImGui::BeginChild("##toolbar", ImVec2(0.0f, 56.0f), ImGuiChildFlags_Borders);
    ImGui::SetCursorPos(ImVec2(18.0f, 13.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.94f, 0.93f, 1.0f));
    ImGui::SetWindowFontScale(1.18f);
    ImGui::TextUnformatted("GLASSLIGHT");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::SameLine(160.0f);
    ImGui::SetCursorPosY(10.0f);
    if (ImGui::Button(runtime_.playing ? "Pause  Space" : "Play  Space", ImVec2(112.0f, 34.0f))) {
        runtime_.playing = !runtime_.playing;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::SliderFloat("##phase", &settings_.phase, 0.0f, 1.0f, "Phase %.3f")) {
        runtime_.playing = false;
        commitEdit(true, actions);
    }

    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(std::max(380.0f, rightEdge - 270.0f));
    if (frame.diagnostics.exportInProgress) {
        ImGui::SetNextItemWidth(130.0f);
        ImGui::ProgressBar(std::clamp(frame.diagnostics.exportProgress, 0.0f, 1.0f),
                           ImVec2(130.0f, 0.0f), "Exporting");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            actions.cancelExportRequested = true;
        }
    } else {
        ImGui::BeginDisabled(!runtime_.generated);
        if (ImGui::Button("Still PNG", ImVec2(82.0f, 34.0f))) {
            actions.exportStillRequested = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Loop MP4", ImVec2(82.0f, 34.0f))) {
            actions.exportVideoRequested = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide  H", ImVec2(76.0f, 34.0f))) {
        runtime_.uiVisible = false;
        if (runtime_.glassPreviewLightbox) {
            runtime_.glassPreviewLightbox = false;
            runtime_.glassPreviewReturningToSync = true;
            runtime_.glassPreviewZoom = 1.0f;
            actions.lightboxChanged = true;
            actions.glassPreviewChanged = true;
        }
        actions.uiVisibilityChanged = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void StudioUi::drawCanvas(const StudioFrame& frame, StudioActions& actions) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(origin, {origin.x + available.x, origin.y + available.y},
                            IM_COL32(9, 13, 17, 255));

    if (runtime_.generated && frame.canvasTexture != ImTextureID_Invalid &&
        frame.canvasWidth != 0 && frame.canvasHeight != 0) {
        const ImVec2 imageSize = fitInside({available.x - 32.0f, available.y - 32.0f},
                                           frame.canvasWidth, frame.canvasHeight);
        ImGui::SetCursorPos({(available.x - imageSize.x) * 0.5f,
                             (available.y - imageSize.y) * 0.5f});
        ImGui::Image(frame.canvasTexture, imageSize);
        return;
    }

    ImGui::SetCursorPosY(std::max(30.0f, available.y * 0.39f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.76f, 0.83f, 0.84f, 1.0f));
    centerText(runtime_.generated ? "Tracing light through the glass..." :
                                    "An invisible glass body. Only its light remains.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    const char* buttonText = runtime_.generated ? "Regenerate" : "Generate composition";
    const ImVec2 buttonSize(184.0f, 42.0f);
    ImGui::SetCursorPosX((available.x - buttonSize.x) * 0.5f);
    if (ImGui::Button(buttonText, buttonSize)) {
        settings_.seed = materializeSeed();
        runtime_.generated = true;
        actions.generateRequested = true;
        commitEdit(true, actions);
    }
}

void StudioUi::drawInspector(const StudioFrame& frame, StudioActions& actions) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    ImGui::SetCursorPos(ImVec2(16.0f, 14.0f));
    ImGui::BeginGroup();
    ImGui::TextDisabled("COMPOSITION");

    ImGui::SetNextItemWidth(-52.0f);
    bool seedEdited = ImGui::InputScalar("##seed", ImGuiDataType_U64, &settings_.seed,
                                         nullptr, nullptr, "%016llX",
                                         ImGuiInputTextFlags_CharsHexadecimal |
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("New", ImVec2(44.0f, 0.0f))) {
        settings_.seed = materializeSeed();
        seedEdited = true;
    }
    if (seedEdited) {
        runtime_.generated = settings_.seed != 0;
        actions.generateRequested = runtime_.generated;
        commitEdit(true, actions);
    }
    if (!runtime_.generated) {
        if (ImGui::Button("Generate", ImVec2(-1.0f, 38.0f))) {
            settings_.seed = materializeSeed();
            runtime_.generated = true;
            actions.generateRequested = true;
            commitEdit(true, actions);
        }
    }

    const bool shapeEdited = shapeCombo(settings_.shapeFamily);
    if (shapeEdited) {
        commitEdit(true, actions);
        actions.glassPreviewChanged = true;
    }
    drawGlassPreviewCard(frame, actions);

    if (ImGui::CollapsingHeader("Form", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool edited = false;
        edited |= ImGui::SliderFloat("Scale", &settings_.shapeScale, 0.35f, 2.0f, "%.2f");
        if (resolveShapeFamily(settings_) == ShapeFamily::Fracture) {
            int shards = static_cast<int>(settings_.fractureShardCount);
            if (ImGui::SliderInt("Shard count", &shards, 3, 16)) {
                settings_.fractureShardCount = static_cast<std::uint32_t>(shards);
                edited = true;
            }
            edited |= fractureMorphologyCombo(settings_.fractureMorphology);
            if (settings_.fractureMorphology == FractureMorphology::Auto) {
                ImGui::TextDisabled("Resolved morphology: %s",
                    fractureMorphologyName(resolveFractureMorphology(settings_)));
            }
            edited |= fractureMotionCombo(settings_.fractureMotion);
            if (settings_.fractureMotion == FractureMotion::Auto) {
                ImGui::TextDisabled("Resolved motion: %s",
                    fractureMotionName(resolveFractureMotion(settings_)));
            }
        }
        int regions = static_cast<int>(settings_.regionCount);
        if (ImGui::SliderInt("Regions", &regions, 1, 24)) {
            settings_.regionCount = static_cast<std::uint32_t>(regions);
            edited = true;
        }
        edited |= ImGui::SliderFloat("Soft transitions", &settings_.regionSoftness,
                                     0.0f, 1.0f, "%.2f");
        commitEdit(edited, actions);
    }

    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool edited = false;
        if (ImGui::BeginCombo("Palette", settings_.palette.name.c_str())) {
            for (const NamedPalette& palette : kPalettes) {
                const bool selected = settings_.palette.name == palette.name;
                if (ImGui::Selectable(palette.name, selected)) {
                    settings_.palette.name = palette.name;
                    settings_.palette.colors = palette.colors;
                    edited = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        const float swatchWidth = std::max(28.0f,
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4.0f) / 5.0f);
        for (std::size_t index = 0; index < settings_.palette.colors.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            Color3& color = settings_.palette.colors[index];
            float values[3]{color.r, color.g, color.b};
            if (index != 0) {
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(swatchWidth);
            if (ImGui::ColorEdit3("##swatch", values,
                                  ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_NoLabel)) {
                color = {values[0], values[1], values[2]};
                settings_.palette.name = "Custom";
                edited = true;
            }
            ImGui::PopID();
        }
        edited |= ImGui::SliderFloat("Color depth", &settings_.colorDepth, 0.0f, 1.0f, "%.2f");
        edited |= ImGui::Checkbox("Include clear regions", &settings_.includeClearRegions);
        commitEdit(edited, actions);
    }

    if (ImGui::CollapsingHeader("Glass", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool edited = ImGui::SliderFloat("Roughness", &settings_.roughness, 0.0f, 0.5f, "%.3f");
        edited |= ImGui::SliderFloat("IOR", &settings_.ior, 1.0f, 2.5f, "%.3f");
        helpMarker("Index of refraction. Typical artistic glass sits near 1.45-1.55.");
        edited |= ImGui::SliderFloat("Dispersion", &settings_.dispersion, 0.0f, 1.0f, "%.2f");
        commitEdit(edited, actions);
    }

    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool edited = ImGui::SliderFloat2("Position", &settings_.lightX, -2.0f, 2.0f, "%.2f");
        edited |= ImGui::SliderFloat("Distance", &settings_.lightDistance, 0.25f, 12.0f, "%.2f");
        edited |= ImGui::SliderFloat("Source size", &settings_.lightRadius, 0.001f, 1.0f, "%.3f");
        edited |= ImGui::SliderFloat("Intensity", &settings_.lightIntensity, 0.0f, 20.0f, "%.2f");
        edited |= ImGui::SliderFloat("Caustic strength", &settings_.projectionStrength,
                                     0.0f, 10.0f, "%.2f");
        commitEdit(edited, actions);
    }

    if (ImGui::CollapsingHeader("Wall & motion", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool edited = wallCombo(settings_.wall);
        edited |= ImGui::SliderFloat("Wall grain", &settings_.wallGrain, 0.0f, 1.0f, "%.2f");
        edited |= ImGui::SliderFloat("Rotation", &settings_.rotationRpm, -30.0f, 30.0f, "%.1f RPM");
        commitEdit(edited, actions);
    }

    ImGui::Separator();
    ImGui::Checkbox("Advanced", &runtime_.showAdvanced);
    if (runtime_.showAdvanced) {
        bool edited = ImGui::Checkbox("Override rotation axis", &settings_.overrideAxis);
        ImGui::BeginDisabled(!settings_.overrideAxis);
        edited |= ImGui::SliderFloat("Axis yaw", &settings_.axisYaw, -180.0f, 180.0f, "%.0f deg");
        edited |= ImGui::SliderFloat("Axis pitch", &settings_.axisPitch, -90.0f, 90.0f, "%.0f deg");
        ImGui::EndDisabled();
        commitEdit(edited, actions);

        if (ImGui::CollapsingHeader("Export settings")) {
            bool exportEdited = qualityCombo(settings_.quality);
            ImGui::TextDisabled("Still image");
            dimensionInput("##still_width", settings_.stillWidth, exportEdited);
            ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine();
            dimensionInput("##still_height", settings_.stillHeight, exportEdited);
            ImGui::TextDisabled("Video loop");
            dimensionInput("##video_width", settings_.videoWidth, exportEdited);
            ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine();
            dimensionInput("##video_height", settings_.videoHeight, exportEdited);
            int fps = static_cast<int>(settings_.videoFps);
            if (ImGui::SliderInt("Frame rate", &fps, 1, 120, "%d fps")) {
                settings_.videoFps = static_cast<std::uint32_t>(fps);
                exportEdited = true;
            }
            commitEdit(exportEdited, actions);
        }

        if (ImGui::CollapsingHeader("Diagnostics")) {
            ImGui::TextWrapped("GPU: %s", frame.diagnostics.gpuName.empty() ?
                               "Not initialized" : frame.diagnostics.gpuName.c_str());
            ImGui::Text("Render: %.2f ms", frame.diagnostics.renderMilliseconds);
            ImGui::Text("Photons: %llu",
                        static_cast<unsigned long long>(frame.diagnostics.accumulatedPhotons));
            ImGui::Text("Hit rate: %.1f%%", frame.diagnostics.hitRate * 100.0f);
            ImGui::ProgressBar(std::clamp(frame.diagnostics.accumulationProgress, 0.0f, 1.0f),
                               ImVec2(-1.0f, 0.0f));
        }
    }

    if (!validationMessage_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.69f, 0.31f, 1.0f));
        ImGui::TextWrapped("%s", validationMessage_.c_str());
        ImGui::PopStyleColor();
    }
    if (!frame.diagnostics.warningMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.53f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", frame.diagnostics.warningMessage.c_str());
        ImGui::PopStyleColor();
    }
    if (!frame.diagnostics.statusMessage.empty()) {
        ImGui::TextDisabled("%s", frame.diagnostics.statusMessage.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Reset options", ImVec2(-1.0f, 0.0f))) {
        resetOptions();
        commitEdit(true, actions);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Restore composition defaults and keep the current seed.");
    }
    if (ImGui::Button("Restore from PNG...", ImVec2(-1.0f, 0.0f))) {
        actions.restoreFromPngRequested = true;
    }
    if (ImGui::Button(runtime_.fullscreen ? "Leave fullscreen  F11" : "Fullscreen  F11",
                      ImVec2(-1.0f, 0.0f))) {
        runtime_.fullscreen = !runtime_.fullscreen;
        actions.fullscreenToggleRequested = true;
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar(2);
}

void StudioUi::drawGlassPreviewCard(const StudioFrame& frame, StudioActions& actions) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const bool expanded = ImGui::CollapsingHeader("Glass object preview");
    if (expanded != runtime_.glassPreviewExpanded) {
        runtime_.glassPreviewExpanded = expanded;
        actions.glassPreviewVisibilityChanged = true;
        actions.glassPreviewChanged = expanded;
    }
    if (!expanded) {
        return;
    }

    ImGui::TextDisabled("Actual family");
    ImGui::SameLine();
    ImGui::TextUnformatted(shapeFamilyName(frame.glassPreviewFamily));

    const char* edgeLabel = "Automatic";
    if (runtime_.glassPreviewEdges == PreviewEdgeMode::Off) edgeLabel = "Off";
    if (runtime_.glassPreviewEdges == PreviewEdgeMode::On) edgeLabel = "On";
    ImGui::SetNextItemWidth(-92.0f);
    if (ImGui::BeginCombo("##preview_edges", edgeLabel)) {
        constexpr std::array<const char*, 3> labels{{"Automatic", "Off", "On"}};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto mode = static_cast<PreviewEdgeMode>(index);
            const bool selected = runtime_.glassPreviewEdges == mode;
            if (ImGui::Selectable(labels[index], selected)) {
                runtime_.glassPreviewEdges = mode;
                actions.glassPreviewChanged = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Edge overlay. Automatic enables edges for geometric families.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open", ImVec2(84.0f, 0.0f))) {
        runtime_.glassPreviewLightbox = true;
        runtime_.glassPreviewReturningToSync = false;
        actions.lightboxChanged = true;
        actions.glassPreviewChanged = true;
    }

    const bool tallLayout = ImGui::GetWindowHeight() >= 790.0f;
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 previewSize{width, tallLayout ? width : width * 0.5625f};
    const std::uint32_t targetWidth = tallLayout ? 256U : 320U;
    const std::uint32_t targetHeight = tallLayout ? 256U : 180U;
    if (runtime_.glassPreviewTargetWidth != targetWidth ||
        runtime_.glassPreviewTargetHeight != targetHeight) {
        runtime_.glassPreviewTargetWidth = targetWidth;
        runtime_.glassPreviewTargetHeight = targetHeight;
        actions.glassPreviewChanged = true;
    }
    drawInteractivePreview(frame, previewSize, true, false, actions);
    ImGui::TextDisabled("Drag to orbit  |  Open to zoom  |  Auto-sync");
}

void StudioUi::drawGlassPreviewLightbox(const StudioFrame& frame,
                                        StudioActions& actions) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags dimmerFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##glass_preview_dimmer", nullptr, dimmerFlags);
    const ImVec2 dimmerOrigin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        dimmerOrigin,
        {dimmerOrigin.x + ImGui::GetWindowWidth(),
         dimmerOrigin.y + ImGui::GetWindowHeight()},
        IM_COL32(2, 5, 7, 205));
    ImGui::InvisibleButton("##glass_preview_dimmer_block", viewport->Size);
    ImGui::End();

    const ImVec2 windowSize{
        std::clamp(viewport->Size.x * 0.72f, 420.0f, 940.0f),
        std::clamp(viewport->Size.y * 0.82f, 430.0f, 900.0f)};
    ImGui::SetNextWindowPos(
        {viewport->Pos.x + viewport->Size.x * 0.5f,
         viewport->Pos.y + viewport->Size.y * 0.5f},
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##glass_preview_lightbox", nullptr, flags);
    ImGui::PopStyleVar(2);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.94f, 0.93f, 1.0f));
    ImGui::Text("GLASS STUDY  /  %s", shapeFamilyName(frame.glassPreviewFamily));
    ImGui::PopStyleColor();
    ImGui::SameLine(std::max(220.0f, ImGui::GetContentRegionMax().x - 278.0f));
    if (ImGui::Button("Reset", ImVec2(76.0f, 0.0f))) {
        runtime_.glassPreviewYaw = 0.0f;
        runtime_.glassPreviewPitch = 0.0f;
        runtime_.glassPreviewZoom = 1.0f;
        runtime_.glassPreviewReturningToSync = false;
        actions.glassPreviewChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(effectiveGlassPreviewEdges() ? "Edges on" : "Edges off",
                      ImVec2(88.0f, 0.0f))) {
        runtime_.glassPreviewEdges = effectiveGlassPreviewEdges()
            ? PreviewEdgeMode::Off : PreviewEdgeMode::On;
        actions.glassPreviewChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(76.0f, 0.0f))) {
        runtime_.glassPreviewLightbox = false;
        runtime_.glassPreviewReturningToSync = true;
        runtime_.glassPreviewZoom = 1.0f;
        actions.lightboxChanged = true;
        actions.glassPreviewChanged = true;
    }

    ImGui::Separator();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0f, std::min(available.x, available.y - 24.0f));
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - side) * 0.5f);
    if (runtime_.glassPreviewTargetWidth != 768U ||
        runtime_.glassPreviewTargetHeight != 768U) {
        runtime_.glassPreviewTargetWidth = 768U;
        runtime_.glassPreviewTargetHeight = 768U;
        actions.glassPreviewChanged = true;
    }
    drawInteractivePreview(frame, {side, side}, true, true, actions);
    ImGui::TextDisabled("Wall paused  |  Glass remains live  |  Esc closes");
    ImGui::End();
}

void StudioUi::drawInteractivePreview(const StudioFrame& frame, const ImVec2& size,
                                      const bool returnToSyncOnRelease,
                                      const bool allowWheelZoom,
                                      StudioActions& actions) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##glass_preview_orbit", size,
                           ImGuiButtonFlags_MouseButtonLeft);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
                            IM_COL32(5, 10, 14, 255), 8.0f);

    if (frame.glassPreviewTexture != ImTextureID_Invalid &&
        frame.glassPreviewWidth != 0 && frame.glassPreviewHeight != 0) {
        const ImVec2 fitted = fitInside(size, frame.glassPreviewWidth,
                                       frame.glassPreviewHeight);
        const ImVec2 imageMin{origin.x + (size.x - fitted.x) * 0.5f,
                              origin.y + (size.y - fitted.y) * 0.5f};
        drawList->AddImage(frame.glassPreviewTexture, imageMin,
                           {imageMin.x + fitted.x, imageMin.y + fitted.y});
    } else {
        const char* message = "Rendering glass preview...";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        drawList->AddText({origin.x + (size.x - textSize.x) * 0.5f,
                           origin.y + (size.y - textSize.y) * 0.5f},
                          IM_COL32(150, 177, 179, 255), message);
    }
    drawList->AddRect(origin, {origin.x + size.x, origin.y + size.y},
                      IM_COL32(83, 150, 147, ImGui::IsItemHovered() ? 210 : 100),
                      8.0f, 0, ImGui::IsItemHovered() ? 2.0f : 1.0f);

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f) {
            runtime_.glassPreviewYaw += delta.x * 0.008f;
            runtime_.glassPreviewPitch = std::clamp(
                runtime_.glassPreviewPitch - delta.y * 0.008f, -1.15f, 1.15f);
            runtime_.glassPreviewOrbiting = true;
            runtime_.glassPreviewReturningToSync = false;
            actions.glassPreviewChanged = true;
        }
    }
    if (ImGui::IsItemDeactivated() && runtime_.glassPreviewOrbiting) {
        runtime_.glassPreviewOrbiting = false;
        runtime_.glassPreviewReturningToSync = returnToSyncOnRelease;
        actions.glassPreviewChanged = true;
    }
    if (allowWheelZoom && ImGui::IsItemHovered() &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        runtime_.glassPreviewZoom = std::clamp(
            runtime_.glassPreviewZoom * std::exp(ImGui::GetIO().MouseWheel * 0.12f),
            0.55f, 2.4f);
        runtime_.glassPreviewReturningToSync = false;
        actions.glassPreviewChanged = true;
    }
}

void StudioUi::commitEdit(const bool edited, StudioActions& actions) {
    if (!edited) {
        return;
    }
    const ValidationResult validation = validate(settings_);
    validationMessage_ = validation.warning;
    actions.compositionChanged = true;
    actions.resetAccumulation = true;
    actions.persistRequested = true;
}

void StudioUi::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.ScrollbarSize = 11.0f;
    style.WindowRounding = 9.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 7.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ImageRounding = 5.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.90f, 0.91f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.56f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.050f, 0.060f, 0.98f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.063f, 0.073f, 0.86f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.040f, 0.057f, 0.066f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.39f, 0.40f, 0.42f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.075f, 0.105f, 0.115f, 0.92f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.19f, 0.19f, 0.96f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.12f, 0.25f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.025f, 0.040f, 0.048f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.050f, 0.100f, 0.105f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.03f, 0.04f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.15f, 0.30f, 0.30f, 0.80f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.38f, 0.91f, 0.82f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.76f, 0.71f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.95f, 0.85f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.27f, 0.27f, 0.88f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.13f, 0.39f, 0.37f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.48f, 0.43f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.24f, 0.24f, 0.78f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.13f, 0.34f, 0.33f, 0.92f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.42f, 0.38f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.33f, 0.34f, 0.55f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.22f, 0.60f, 0.56f, 0.25f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.33f, 0.83f, 0.75f, 1.00f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.53f, 0.96f, 0.87f, 1.00f);
}

} // namespace glasslight
