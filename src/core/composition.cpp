#include "core/composition.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>

namespace glasslight {
namespace {

using Json = nlohmann::json;

template <typename T>
bool clampValue(T& value, const T minimum, const T maximum) {
    const T replacement = std::clamp(value, minimum, maximum);
    if (replacement == value) {
        return false;
    }
    value = replacement;
    return true;
}

bool clampFinite(float& value, const float minimum, const float maximum,
                 const float fallback) {
    if (!std::isfinite(value)) {
        value = fallback;
        return true;
    }
    return clampValue(value, minimum, maximum);
}

template <typename Enum>
bool enumInRange(const Enum value, const Enum first, const Enum last) {
    const auto raw = static_cast<std::uint32_t>(value);
    return raw >= static_cast<std::uint32_t>(first) &&
           raw <= static_cast<std::uint32_t>(last);
}

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

template <typename T>
void readIfPresent(const Json& object, const char* key, T& destination) {
    const auto it = object.find(key);
    if (it != object.end() && !it->is_null()) {
        destination = it->get<T>();
    }
}

template <typename Enum>
void readEnum(const Json& object, const char* key, Enum& destination) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return;
    }
    destination = static_cast<Enum>(it->get<std::uint32_t>());
}

Json colorToJson(const Color3& color) {
    return Json::array({color.r, color.g, color.b});
}

void readColor(const Json& source, Color3& destination) {
    if (!source.is_array() || source.size() != 3) {
        throw Json::type_error::create(302, "palette color must be an RGB array", &source);
    }
    destination.r = source.at(0).get<float>();
    destination.g = source.at(1).get<float>();
    destination.b = source.at(2).get<float>();
}

} // namespace

ValidationResult validate(CompositionSettings& settings) {
    ValidationResult result;
    auto noteChange = [&result](const bool changed) { result.changed |= changed; };

    if (settings.schemaVersion != CompositionSettings::kSchemaVersion) {
        settings.schemaVersion = CompositionSettings::kSchemaVersion;
        result.changed = true;
    }
    if (!enumInRange(settings.shapeFamily, ShapeFamily::Auto, ShapeFamily::Fracture)) {
        settings.shapeFamily = ShapeFamily::Auto;
        result.changed = true;
    }
    if (!enumInRange(settings.wall, WallPreset::WarmWhitePaint, WallPreset::Charcoal)) {
        settings.wall = WallPreset::WarmWhitePaint;
        result.changed = true;
    }
    if (!enumInRange(settings.quality, QualityPreset::Draft, QualityPreset::High)) {
        settings.quality = QualityPreset::Standard;
        result.changed = true;
    }
    if (!enumInRange(settings.fractureMorphology, FractureMorphology::Auto,
                     FractureMorphology::Hybrid)) {
        settings.fractureMorphology = FractureMorphology::Auto;
        result.changed = true;
    }
    if (!enumInRange(settings.fractureMotion, FractureMotion::Auto,
                     FractureMotion::BreakAndReform)) {
        settings.fractureMotion = FractureMotion::Auto;
        result.changed = true;
    }

    noteChange(clampFinite(settings.shapeScale, 0.35f, 2.0f, 1.0f));
    noteChange(clampValue(settings.fractureShardCount, 3U, 16U));
    noteChange(clampValue(settings.regionCount, 1U, 24U));
    noteChange(clampFinite(settings.regionSoftness, 0.0f, 1.0f, 0.18f));
    noteChange(clampFinite(settings.colorDepth, 0.0f, 1.0f, 0.65f));
    for (Color3& color : settings.palette.colors) {
        noteChange(clampFinite(color.r, 0.0f, 1.0f, 1.0f));
        noteChange(clampFinite(color.g, 0.0f, 1.0f, 1.0f));
        noteChange(clampFinite(color.b, 0.0f, 1.0f, 1.0f));
    }
    if (settings.palette.name.size() > 80) {
        settings.palette.name.resize(80);
        result.changed = true;
    }

    noteChange(clampFinite(settings.ior, 1.0f, 2.5f, 1.52f));
    noteChange(clampFinite(settings.roughness, 0.0f, 0.5f, 0.06f));
    noteChange(clampFinite(settings.dispersion, 0.0f, 1.0f, 0.12f));
    noteChange(clampFinite(settings.lightX, -2.0f, 2.0f, -0.35f));
    noteChange(clampFinite(settings.lightY, -2.0f, 2.0f, 0.25f));
    noteChange(clampFinite(settings.lightDistance, 0.25f, 12.0f, 2.6f));
    noteChange(clampFinite(settings.lightRadius, 0.001f, 1.0f, 0.08f));
    noteChange(clampFinite(settings.lightIntensity, 0.0f, 20.0f, 3.0f));
    noteChange(clampFinite(settings.projectionStrength, 0.0f, 10.0f, 1.5f));
    noteChange(clampFinite(settings.wallGrain, 0.0f, 1.0f, 0.12f));

    noteChange(clampFinite(settings.rotationRpm, -60.0f, 60.0f, 6.0f));
    if (!std::isfinite(settings.phase)) {
        settings.phase = 0.0f;
        result.changed = true;
    } else {
        const float wrapped = settings.phase - std::floor(settings.phase);
        if (wrapped != settings.phase) {
            settings.phase = wrapped;
            result.changed = true;
        }
    }
    noteChange(clampFinite(settings.axisYaw, -180.0f, 180.0f, 0.0f));
    noteChange(clampFinite(settings.axisPitch, -90.0f, 90.0f, 0.0f));

    noteChange(clampValue(settings.stillWidth, 16U, 4096U));
    noteChange(clampValue(settings.stillHeight, 16U, 4096U));
    noteChange(clampValue(settings.videoWidth, 16U, 4096U));
    noteChange(clampValue(settings.videoHeight, 16U, 4096U));
    noteChange(clampValue(settings.videoFps, 1U, 120U));

    if (result.changed) {
        result.warning = "Some composition values were outside supported ranges and were adjusted.";
    }
    return result;
}

std::uint64_t materializeSeed() {
    std::random_device randomDevice;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::uint64_t entropy =
        (static_cast<std::uint64_t>(randomDevice()) << 32U) ^ randomDevice() ^ now;
    const std::uint64_t seed = splitMix64(entropy);
    return seed == 0 ? 0x474c415353454544ULL : seed;
}

std::uint32_t shaderSeed(const std::uint64_t seed, const std::uint32_t stream) {
    const std::uint64_t mixed = splitMix64(seed ^
        (static_cast<std::uint64_t>(stream) * 0x9e3779b97f4a7c15ULL));
    const std::uint32_t folded = static_cast<std::uint32_t>(mixed ^ (mixed >> 32U));
    return folded == 0 ? 0x6d2b79f5U : folded;
}

ShapeFamily resolveShapeFamily(const CompositionSettings& settings) {
    if (settings.shapeFamily != ShapeFamily::Auto) {
        return settings.shapeFamily;
    }
    return static_cast<ShapeFamily>(1U + shaderSeed(settings.seed, 0) % 6U);
}

FractureMorphology resolveFractureMorphology(const CompositionSettings& settings) {
    if (settings.fractureMorphology != FractureMorphology::Auto) {
        return settings.fractureMorphology;
    }
    return static_cast<FractureMorphology>(1U + shaderSeed(settings.seed, 21U) % 3U);
}

FractureMotion resolveFractureMotion(const CompositionSettings& settings) {
    if (settings.fractureMotion != FractureMotion::Auto) {
        return settings.fractureMotion;
    }
    return static_cast<FractureMotion>(1U + shaderSeed(settings.seed, 22U) % 3U);
}

double loopDurationSeconds(const CompositionSettings& settings) {
    if (!std::isfinite(settings.rotationRpm) || std::abs(settings.rotationRpm) < 1.0e-6f) {
        return std::numeric_limits<double>::infinity();
    }
    return 60.0 / std::abs(static_cast<double>(settings.rotationRpm));
}

double photonsPerMegapixel(const QualityPreset quality) {
    switch (quality) {
    case QualityPreset::Draft: return 500'000.0;
    case QualityPreset::Standard: return 2'000'000.0;
    case QualityPreset::High: return 8'000'000.0;
    }
    return 2'000'000.0;
}

std::string serializeSettings(const CompositionSettings& input) {
    CompositionSettings settings = input;
    validate(settings);

    Json colors = Json::array();
    for (const Color3& color : settings.palette.colors) {
        colors.push_back(colorToJson(color));
    }
    const Json payload = {
        {"color", {
            {"depth", settings.colorDepth},
            {"include_clear_regions", settings.includeClearRegions},
            {"palette", {{"colors", colors}, {"name", settings.palette.name}}},
            {"region_count", settings.regionCount},
            {"region_softness", settings.regionSoftness}
        }},
        {"export", {
            {"quality", static_cast<std::uint32_t>(settings.quality)},
            {"still", {{"height", settings.stillHeight}, {"width", settings.stillWidth}}},
            {"video", {{"fps", settings.videoFps}, {"height", settings.videoHeight},
                       {"width", settings.videoWidth}}}
        }},
        {"glass", {
            {"dispersion", settings.dispersion},
            {"ior", settings.ior},
            {"roughness", settings.roughness}
        }},
        {"light", {
            {"distance", settings.lightDistance},
            {"intensity", settings.lightIntensity},
            {"position", Json::array({settings.lightX, settings.lightY})},
            {"projection_strength", settings.projectionStrength},
            {"radius", settings.lightRadius}
        }},
        {"rotation", {
            {"axis_pitch", settings.axisPitch},
            {"axis_yaw", settings.axisYaw},
            {"override_axis", settings.overrideAxis},
            {"phase", settings.phase},
            {"rpm", settings.rotationRpm}
        }},
        {"schema_version", settings.schemaVersion},
        {"seed", settings.seed},
        {"shape", {
            {"family", static_cast<std::uint32_t>(settings.shapeFamily)},
            {"fracture", {
                {"morphology", static_cast<std::uint32_t>(settings.fractureMorphology)},
                {"motion", static_cast<std::uint32_t>(settings.fractureMotion)},
                {"shard_count", settings.fractureShardCount}
            }},
            {"scale", settings.shapeScale}
        }},
        {"wall", {
            {"grain", settings.wallGrain},
            {"preset", static_cast<std::uint32_t>(settings.wall)}
        }}
    };
    return payload.dump(2) + '\n';
}

bool deserializeSettings(const std::string& payload, CompositionSettings& settings,
                         std::string& error) {
    try {
        const Json root = Json::parse(payload);
        if (!root.is_object()) {
            error = "Composition payload must be a JSON object.";
            return false;
        }
        const std::uint32_t schemaVersion = root.value("schema_version", 1U);
        if (schemaVersion == 0 || schemaVersion > CompositionSettings::kSchemaVersion) {
            error = "Unsupported composition schema version " +
                    std::to_string(schemaVersion) + ".";
            return false;
        }

        CompositionSettings parsed;
        parsed.schemaVersion = schemaVersion;
        readIfPresent(root, "seed", parsed.seed);
        if (const auto it = root.find("shape"); it != root.end()) {
            readEnum(*it, "family", parsed.shapeFamily);
            readIfPresent(*it, "scale", parsed.shapeScale);
            if (schemaVersion >= 2U) {
                if (const auto fracture = it->find("fracture"); fracture != it->end()) {
                    readIfPresent(*fracture, "shard_count", parsed.fractureShardCount);
                    readEnum(*fracture, "morphology", parsed.fractureMorphology);
                    readEnum(*fracture, "motion", parsed.fractureMotion);
                }
            }
        }
        if (const auto it = root.find("color"); it != root.end()) {
            readIfPresent(*it, "region_count", parsed.regionCount);
            readIfPresent(*it, "region_softness", parsed.regionSoftness);
            readIfPresent(*it, "depth", parsed.colorDepth);
            readIfPresent(*it, "include_clear_regions", parsed.includeClearRegions);
            if (const auto palette = it->find("palette"); palette != it->end()) {
                readIfPresent(*palette, "name", parsed.palette.name);
                if (const auto colors = palette->find("colors"); colors != palette->end()) {
                    if (!colors->is_array() || colors->size() != parsed.palette.colors.size()) {
                        error = "Palette must contain exactly five colors.";
                        return false;
                    }
                    for (std::size_t i = 0; i < parsed.palette.colors.size(); ++i) {
                        readColor(colors->at(i), parsed.palette.colors[i]);
                    }
                }
            }
        }
        if (const auto it = root.find("glass"); it != root.end()) {
            readIfPresent(*it, "ior", parsed.ior);
            readIfPresent(*it, "roughness", parsed.roughness);
            readIfPresent(*it, "dispersion", parsed.dispersion);
        }
        if (const auto it = root.find("light"); it != root.end()) {
            readIfPresent(*it, "distance", parsed.lightDistance);
            readIfPresent(*it, "radius", parsed.lightRadius);
            readIfPresent(*it, "intensity", parsed.lightIntensity);
            readIfPresent(*it, "projection_strength", parsed.projectionStrength);
            if (const auto position = it->find("position"); position != it->end()) {
                if (!position->is_array() || position->size() != 2) {
                    error = "Light position must be a two-element array.";
                    return false;
                }
                parsed.lightX = position->at(0).get<float>();
                parsed.lightY = position->at(1).get<float>();
            }
        }
        if (const auto it = root.find("wall"); it != root.end()) {
            readEnum(*it, "preset", parsed.wall);
            readIfPresent(*it, "grain", parsed.wallGrain);
        }
        if (const auto it = root.find("rotation"); it != root.end()) {
            readIfPresent(*it, "rpm", parsed.rotationRpm);
            readIfPresent(*it, "phase", parsed.phase);
            readIfPresent(*it, "override_axis", parsed.overrideAxis);
            readIfPresent(*it, "axis_yaw", parsed.axisYaw);
            readIfPresent(*it, "axis_pitch", parsed.axisPitch);
        }
        if (const auto it = root.find("export"); it != root.end()) {
            readEnum(*it, "quality", parsed.quality);
            if (const auto still = it->find("still"); still != it->end()) {
                readIfPresent(*still, "width", parsed.stillWidth);
                readIfPresent(*still, "height", parsed.stillHeight);
            }
            if (const auto video = it->find("video"); video != it->end()) {
                readIfPresent(*video, "width", parsed.videoWidth);
                readIfPresent(*video, "height", parsed.videoHeight);
                readIfPresent(*video, "fps", parsed.videoFps);
            }
        }

        const ValidationResult validation = validate(parsed);
        settings = std::move(parsed);
        error = validation.warning;
        return true;
    } catch (const Json::exception& exception) {
        error = std::string("Invalid composition JSON: ") + exception.what();
        return false;
    } catch (const std::exception& exception) {
        error = std::string("Could not read composition: ") + exception.what();
        return false;
    }
}

const char* shapeFamilyName(const ShapeFamily family) {
    switch (family) {
    case ShapeFamily::Auto: return "Auto";
    case ShapeFamily::Pebble: return "Pebble";
    case ShapeFamily::Lens: return "Lens";
    case ShapeFamily::Ribbon: return "Ribbon";
    case ShapeFamily::FacetedVessel: return "Faceted Vessel";
    case ShapeFamily::CutCrystal: return "Cut Crystal";
    case ShapeFamily::Fracture: return "Fracture";
    }
    return "Auto";
}

const char* fractureMorphologyName(const FractureMorphology morphology) {
    switch (morphology) {
    case FractureMorphology::Auto: return "Auto";
    case FractureMorphology::Growth: return "Growth";
    case FractureMorphology::Exploded: return "Exploded";
    case FractureMorphology::Hybrid: return "Hybrid";
    }
    return "Auto";
}

const char* fractureMotionName(const FractureMotion motion) {
    switch (motion) {
    case FractureMotion::Auto: return "Auto";
    case FractureMotion::Rigid: return "Rigid";
    case FractureMotion::GrowAndRecede: return "Grow & Recede";
    case FractureMotion::BreakAndReform: return "Break & Reform";
    }
    return "Auto";
}

const char* wallPresetName(const WallPreset wall) {
    switch (wall) {
    case WallPreset::WarmWhitePaint: return "Warm White Paint";
    case WallPreset::NeutralGallery: return "Neutral Gallery";
    case WallPreset::CoolPlaster: return "Cool Plaster";
    case WallPreset::Charcoal: return "Charcoal";
    }
    return "Warm White Paint";
}

const char* qualityPresetName(const QualityPreset quality) {
    switch (quality) {
    case QualityPreset::Draft: return "Draft";
    case QualityPreset::Standard: return "Standard";
    case QualityPreset::High: return "High";
    }
    return "Standard";
}

} // namespace glasslight
