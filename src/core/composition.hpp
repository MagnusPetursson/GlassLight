#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace glasslight {

enum class ShapeFamily : std::uint32_t {
    Auto = 0,
    Pebble = 1,
    Lens = 2,
    Ribbon = 3,
    FacetedVessel = 4,
    CutCrystal = 5,
    Fracture = 6
};
enum class WallPreset : std::uint32_t {
    WarmWhitePaint = 0,
    NeutralGallery = 1,
    CoolPlaster = 2,
    Charcoal = 3
};
enum class QualityPreset : std::uint32_t { Draft = 0, Standard = 1, High = 2 };
enum class FractureMorphology : std::uint32_t {
    Auto = 0,
    Growth = 1,
    Exploded = 2,
    Hybrid = 3
};
enum class FractureMotion : std::uint32_t {
    Auto = 0,
    Rigid = 1,
    GrowAndRecede = 2,
    BreakAndReform = 3
};

struct Color3 {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

struct Palette {
    std::string name = "Cathedral Dawn";
    std::array<Color3, 5> colors{{
        {0.20f, 0.36f, 0.72f},
        {0.20f, 0.67f, 0.56f},
        {0.95f, 0.58f, 0.22f},
        {0.78f, 0.28f, 0.38f},
        {0.48f, 0.30f, 0.68f}
    }};
};

struct CompositionSettings {
    static constexpr std::uint32_t kSchemaVersion = 2;

    std::uint32_t schemaVersion = kSchemaVersion;
    std::uint64_t seed = 0;

    ShapeFamily shapeFamily = ShapeFamily::Auto;
    float shapeScale = 1.0f;
    std::uint32_t fractureShardCount = 9;
    FractureMorphology fractureMorphology = FractureMorphology::Auto;
    FractureMotion fractureMotion = FractureMotion::Auto;

    std::uint32_t regionCount = 7;
    float regionSoftness = 0.18f;
    float colorDepth = 0.78f;
    bool includeClearRegions = true;
    Palette palette{};

    float ior = 1.52f;
    float roughness = 0.035f;
    float dispersion = 0.32f;

    float lightX = -0.35f;
    float lightY = 0.25f;
    float lightDistance = 2.6f;
    float lightRadius = 0.045f;
    float lightIntensity = 4.5f;
    float projectionStrength = 2.4f;

    WallPreset wall = WallPreset::WarmWhitePaint;
    float wallGrain = 0.08f;

    float rotationRpm = 6.0f;
    float phase = 0.0f;
    bool overrideAxis = false;
    float axisYaw = 0.0f;
    float axisPitch = 0.0f;

    QualityPreset quality = QualityPreset::Standard;
    std::uint32_t stillWidth = 3840;
    std::uint32_t stillHeight = 2160;
    std::uint32_t videoWidth = 1920;
    std::uint32_t videoHeight = 1080;
    std::uint32_t videoFps = 30;
};

struct ValidationResult {
    bool changed = false;
    std::string warning;
};

ValidationResult validate(CompositionSettings& settings);
std::uint64_t materializeSeed();
std::uint32_t shaderSeed(std::uint64_t seed, std::uint32_t stream = 0);
ShapeFamily resolveShapeFamily(const CompositionSettings& settings);
FractureMorphology resolveFractureMorphology(const CompositionSettings& settings);
FractureMotion resolveFractureMotion(const CompositionSettings& settings);
double loopDurationSeconds(const CompositionSettings& settings);
double photonsPerMegapixel(QualityPreset quality);

std::string serializeSettings(const CompositionSettings& settings);
bool deserializeSettings(const std::string& payload, CompositionSettings& settings,
                         std::string& error);

const char* shapeFamilyName(ShapeFamily family);
const char* fractureMorphologyName(FractureMorphology morphology);
const char* fractureMotionName(FractureMotion motion);
const char* wallPresetName(WallPreset wall);
const char* qualityPresetName(QualityPreset quality);

} // namespace glasslight
