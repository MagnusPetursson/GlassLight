#include "core/composition.hpp"
#include "studio/studio_ui.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    glasslight::CompositionSettings composition;
    composition.seed = 0x48494444454e5549ULL;
    composition.shapeFamily = glasslight::ShapeFamily::Fracture;
    composition.stillWidth = 2048;
    composition.stillHeight = 1152;

    glasslight::StudioUi studio(composition);
    const std::string exportSettings = glasslight::serializeSettings(studio.settings());

    studio.mutableRuntime().uiVisible = false;
    studio.mutableRuntime().fullscreen = true;
    studio.mutableRuntime().showAdvanced = true;
    studio.mutableRuntime().glassPreviewExpanded = false;
    studio.mutableRuntime().glassPreviewLightbox = true;
    studio.mutableRuntime().glassPreviewEdges = glasslight::PreviewEdgeMode::On;
    studio.mutableRuntime().glassPreviewYaw = 1.25f;
    studio.mutableRuntime().glassPreviewPitch = -0.4f;
    studio.mutableRuntime().glassPreviewZoom = 1.7f;
    studio.mutableRuntime().glassPreviewOrbiting = true;
    studio.mutableRuntime().glassPreviewTargetWidth = 640u;
    studio.mutableRuntime().glassPreviewTargetHeight = 640u;
    expect(glasslight::serializeSettings(studio.settings()) == exportSettings,
           "UI, glass preview, fullscreen, and debug state must not enter export settings");

    studio.mutableRuntime().uiVisible = true;
    studio.mutableRuntime().fullscreen = false;
    studio.mutableRuntime().showAdvanced = false;
    studio.mutableRuntime().glassPreviewExpanded = true;
    studio.mutableRuntime().glassPreviewLightbox = false;
    studio.mutableRuntime().glassPreviewEdges = glasslight::PreviewEdgeMode::Automatic;
    studio.mutableRuntime().glassPreviewYaw = 0.0f;
    studio.mutableRuntime().glassPreviewPitch = 0.0f;
    studio.mutableRuntime().glassPreviewZoom = 1.0f;
    studio.mutableRuntime().glassPreviewOrbiting = false;
    studio.mutableRuntime().glassPreviewTargetWidth = 256u;
    studio.mutableRuntime().glassPreviewTargetHeight = 256u;
    expect(glasslight::serializeSettings(studio.settings()) == exportSettings,
           "restoring presentation state must not alter the composition");

    expect(studio.settings().stillWidth == 2048u &&
               studio.settings().stillHeight == 1152u,
           "export dimensions should be composition data, independent of UI state");

    glasslight::CompositionSettings edited;
    edited.seed = 0x52455345544f5054ULL;
    edited.shapeFamily = glasslight::ShapeFamily::CutCrystal;
    edited.fractureShardCount = 16;
    edited.fractureMorphology = glasslight::FractureMorphology::Exploded;
    edited.fractureMotion = glasslight::FractureMotion::BreakAndReform;
    edited.shapeScale = 1.8f;
    edited.regionCount = 19;
    edited.palette.name = "Custom";
    edited.palette.colors[0] = {0.01f, 0.02f, 0.03f};
    edited.ior = 2.1f;
    edited.roughness = 0.42f;
    edited.lightIntensity = 12.0f;
    edited.wall = glasslight::WallPreset::Charcoal;
    edited.rotationRpm = -17.0f;
    edited.phase = 0.7f;
    edited.quality = glasslight::QualityPreset::High;
    edited.stillWidth = 8192;

    glasslight::StudioUi resetStudio(edited);
    resetStudio.mutableRuntime().fullscreen = true;
    resetStudio.mutableRuntime().showAdvanced = true;
    resetStudio.mutableRuntime().glassPreviewExpanded = false;
    resetStudio.mutableRuntime().glassPreviewYaw = 0.8f;
    resetStudio.resetOptions();

    glasslight::CompositionSettings expectedDefaults;
    expectedDefaults.seed = edited.seed;
    expect(glasslight::serializeSettings(resetStudio.settings()) ==
               glasslight::serializeSettings(expectedDefaults),
           "reset options should restore every composition default and retain the seed");
    expect(resetStudio.runtime().generated,
           "reset options should keep a seeded composition generated");
    expect(resetStudio.runtime().fullscreen && resetStudio.runtime().showAdvanced &&
               !resetStudio.runtime().glassPreviewExpanded &&
               resetStudio.runtime().glassPreviewYaw == 0.8f,
           "reset options should not alter presentation-only session state");

    if (failures == 0) {
        std::cout << "Studio state separation tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
