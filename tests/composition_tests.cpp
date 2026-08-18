#include "core/composition.hpp"
#include "core/fracture_layout.hpp"
#include "core/persistence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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
    constexpr std::array concreteFamilies{
        glasslight::ShapeFamily::Pebble,
        glasslight::ShapeFamily::Lens,
        glasslight::ShapeFamily::Ribbon,
        glasslight::ShapeFamily::FacetedVessel,
        glasslight::ShapeFamily::CutCrystal,
        glasslight::ShapeFamily::Fracture};

    glasslight::CompositionSettings settings;
    settings.seed = 0x123456789abcdef0ULL;
    settings.shapeFamily = glasslight::ShapeFamily::Ribbon;
    settings.phase = 0.375f;
    settings.palette.name = "Round trip";
    settings.palette.colors[2] = {0.12f, 0.34f, 0.56f};
    settings.fractureShardCount = 16;
    settings.fractureMorphology = glasslight::FractureMorphology::Hybrid;
    settings.fractureMotion = glasslight::FractureMotion::BreakAndReform;

    const std::string serialized = glasslight::serializeSettings(settings);
    glasslight::CompositionSettings restored;
    std::string error;
    expect(glasslight::deserializeSettings(serialized, restored, error),
           "canonical settings should deserialize: " + error);
    expect(glasslight::serializeSettings(restored) == serialized,
           "settings serialization should be canonical and lossless");
    expect(restored.seed == settings.seed, "64-bit seed should round-trip exactly");
    expect(restored.shapeFamily == glasslight::ShapeFamily::Ribbon,
           "shape family should round-trip");
    expect(restored.schemaVersion == 2u && restored.fractureShardCount == 16u &&
               restored.fractureMorphology == glasslight::FractureMorphology::Hybrid &&
               restored.fractureMotion == glasslight::FractureMotion::BreakAndReform,
           "v2 Fracture controls should round-trip canonically");

    const std::string v1 =
        R"({"schema_version":1,"seed":77,"shape":{"family":6,"scale":1.1}})";
    expect(glasslight::deserializeSettings(v1, restored, error),
           "v1 compositions should migrate: " + error);
    expect(restored.schemaVersion == 2u && restored.fractureShardCount == 9u &&
               restored.fractureMorphology == glasslight::FractureMorphology::Auto &&
               restored.fractureMotion == glasslight::FractureMotion::Auto,
           "v1 migration should install safe Fracture defaults");
    expect(glasslight::serializeSettings(restored).find("\"schema_version\": 2") !=
               std::string::npos,
           "migrated settings should serialize as canonical v2");

    expect(glasslight::shaderSeed(settings.seed, 7) ==
               glasslight::shaderSeed(settings.seed, 7),
           "shader seed derivation should be deterministic");
    expect(glasslight::shaderSeed(settings.seed, 7) !=
               glasslight::shaderSeed(settings.seed, 8),
           "independent shader streams should differ");

    glasslight::CompositionSettings automatic = settings;
    automatic.shapeFamily = glasslight::ShapeFamily::Auto;
    const auto resolved = glasslight::resolveShapeFamily(automatic);
    expect(resolved == glasslight::resolveShapeFamily(automatic),
           "automatic shape choice should be seed-stable");
    expect(resolved != glasslight::ShapeFamily::Auto,
           "automatic shape should resolve to a concrete family");
    expect(static_cast<std::uint32_t>(resolved) >=
               static_cast<std::uint32_t>(glasslight::ShapeFamily::Pebble) &&
               static_cast<std::uint32_t>(resolved) <=
               static_cast<std::uint32_t>(glasslight::ShapeFamily::Fracture),
           "automatic shape should resolve across the full concrete family range");

    std::array<bool, 6> seen{};
    for (std::uint64_t seed = 0; seed < 4096; ++seed) {
        automatic.seed = seed;
        const auto family = glasslight::resolveShapeFamily(automatic);
        seen[static_cast<std::size_t>(family) - 1] = true;
    }
    expect(std::all_of(seen.begin(), seen.end(), [](const bool value) { return value; }),
           "automatic shape selection should reach all six concrete families");

    for (const auto family : concreteFamilies) {
        settings.shapeFamily = family;
        const std::string familyJson = glasslight::serializeSettings(settings);
        expect(glasslight::deserializeSettings(familyJson, restored, error),
               "concrete shape family should deserialize: " + error);
        expect(restored.shapeFamily == family, "concrete shape family should round-trip");
        expect(std::string(glasslight::shapeFamilyName(family)) != "Auto",
               "concrete shape family should expose its display name");

        const auto firstResolution = glasslight::resolveShapeFamily(settings);
        settings.seed ^= 0xd1b54a32d192ed03ULL;
        expect(glasslight::resolveShapeFamily(settings) == firstResolution,
               "an explicit family must not change when the seed changes");
        settings.seed ^= 0xd1b54a32d192ed03ULL;
    }

    constexpr std::array expectedNames{
        "Auto", "Pebble", "Lens", "Ribbon", "Faceted Vessel", "Cut Crystal",
        "Fracture"};
    for (std::uint32_t raw = 0; raw < expectedNames.size(); ++raw) {
        expect(std::string(glasslight::shapeFamilyName(
                   static_cast<glasslight::ShapeFamily>(raw))) == expectedNames[raw],
               "every serialized shape enum should have a stable display name");
    }

    glasslight::CompositionSettings invalid;
    invalid.regionCount = 999;
    invalid.ior = -1.0f;
    invalid.stillWidth = 0;
    invalid.videoFps = 999;
    const auto validation = glasslight::validate(invalid);
    expect(validation.changed, "out-of-range values should be normalized");
    expect(invalid.regionCount <= 24 && invalid.ior >= 1.0f &&
               invalid.stillWidth > 0 && invalid.videoFps <= 120,
           "validation should enforce public UI/render limits");
    invalid.shapeFamily = static_cast<glasslight::ShapeFamily>(999u);
    invalid.fractureShardCount = 99u;
    invalid.fractureMorphology = static_cast<glasslight::FractureMorphology>(99u);
    invalid.fractureMotion = static_cast<glasslight::FractureMotion>(99u);
    expect(glasslight::validate(invalid).changed &&
               invalid.shapeFamily == glasslight::ShapeFamily::Auto &&
               invalid.fractureShardCount == 16u &&
               invalid.fractureMorphology == glasslight::FractureMorphology::Auto &&
               invalid.fractureMotion == glasslight::FractureMotion::Auto,
           "unknown enums and Fracture counts should normalize safely");

    glasslight::CompositionSettings fracture;
    fracture.shapeFamily = glasslight::ShapeFamily::Fracture;
    std::array<std::uint32_t, 4> morphologyCounts{};
    std::array<std::uint32_t, 4> motionCounts{};
    for (std::uint64_t seed = 0; seed < 12'000; ++seed) {
        fracture.seed = seed;
        const auto morphology = glasslight::resolveFractureMorphology(fracture);
        const auto motion = glasslight::resolveFractureMotion(fracture);
        expect(morphology == glasslight::resolveFractureMorphology(fracture) &&
                   motion == glasslight::resolveFractureMotion(fracture),
               "Auto Fracture resolution should be deterministic");
        ++morphologyCounts[static_cast<std::size_t>(morphology)];
        ++motionCounts[static_cast<std::size_t>(motion)];
    }
    for (std::size_t value = 1; value <= 3; ++value) {
        expect(morphologyCounts[value] > 3600u && morphologyCounts[value] < 4400u,
               "Auto morphology should have balanced thirds");
        expect(motionCounts[value] > 3600u && motionCounts[value] < 4400u,
               "Auto motion should have balanced thirds");
    }
    fracture.seed = 5u;
    fracture.fractureMorphology = glasslight::FractureMorphology::Exploded;
    fracture.fractureMotion = glasslight::FractureMotion::Rigid;
    const auto forcedMorphology = glasslight::resolveFractureMorphology(fracture);
    const auto forcedMotion = glasslight::resolveFractureMotion(fracture);
    fracture.seed = 987654321u;
    expect(glasslight::resolveFractureMorphology(fracture) == forcedMorphology &&
               glasslight::resolveFractureMotion(fracture) == forcedMotion,
           "explicit Fracture overrides should take precedence over seed resolution");
    fracture.fractureMorphology = glasslight::FractureMorphology::Growth;
    fracture.fractureMotion = glasslight::FractureMotion::Rigid;
    for (const std::uint32_t count : {3u, 9u, 16u}) {
        fracture.fractureShardCount = count;
        const auto layout = glasslight::buildFractureLayout(fracture);
        expect(layout.shardCount == count, "layout should produce the exact shard count");
        std::array<bool, 4> profiles{};
        for (std::uint32_t index = 0; index < layout.shardCount; ++index) {
            const auto& shard = layout.shards[index];
            const float axisLength = std::sqrt(shard.axis[0] * shard.axis[0] +
                                               shard.axis[1] * shard.axis[1] +
                                               shard.axis[2] * shard.axis[2]);
            expect(std::isfinite(axisLength) && std::abs(axisLength - 1.0f) < 1.0e-5f &&
                       std::isfinite(shard.length) && std::isfinite(shard.radius),
                   "shard descriptors should be finite and normalized");
            expect(shard.tipFraction >= 0.25f && shard.length > shard.radius * 2.0f,
                   "every shard should be anisotropic and visibly pointed");
            expect(shard.planeCount == shard.polygonSides * 2u + 1u,
                   "each convex crystal should contain side, tip, and base planes");
            profiles[static_cast<std::size_t>(shard.profile)] = true;
        }
        expect(profiles[0] && profiles[1] && profiles[3],
               "every count should guarantee quartz, column, and blade profiles");
        if (count >= 4u) {
            expect(profiles[2], "four or more shards should guarantee a stout crystal");
        }
    }

    fracture.seed = 0x9988776655443322ULL;
    fracture.fractureShardCount = 9u;
    fracture.phase = 0.0f;
    const auto seamStart = glasslight::buildFractureLayout(fracture);
    fracture.phase = 1.0f;
    const auto seamEnd = glasslight::buildFractureLayout(fracture);
    for (std::uint32_t index = 0; index < seamStart.shardCount; ++index) {
        expect(seamStart.shards[index].base == seamEnd.shards[index].base &&
                   seamStart.shards[index].axis == seamEnd.shards[index].axis &&
                   seamStart.shards[index].length == seamEnd.shards[index].length,
               "phase zero and one layouts should meet exactly at the loop seam");
    }

    fracture.phase = 0.25f;
    const auto connectedGrowth = glasslight::buildFractureLayout(fracture);
    for (std::uint32_t index = 1; index < connectedGrowth.shardCount; ++index) {
        const auto& dominant = connectedGrowth.shards[0];
        const auto& shard = connectedGrowth.shards[index];
        const float dx = shard.base[0] - dominant.base[0];
        const float dy = shard.base[1] - dominant.base[1];
        const float dz = shard.base[2] - dominant.base[2];
        const float baseDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        expect(baseDistance < dominant.radius + shard.radius + 0.035f,
               "Growth roots should overlap the dominant spire into one connected cluster");
    }

    fracture.phase = 0.5f;
    fracture.fractureMorphology = glasslight::FractureMorphology::Exploded;
    fracture.fractureMotion = glasslight::FractureMotion::BreakAndReform;
    const auto exploded = glasslight::buildFractureLayout(fracture);
    for (std::uint32_t index = 0; index < exploded.shardCount; ++index) {
        const auto& base = exploded.shards[index].base;
        expect(std::sqrt(base[0] * base[0] + base[2] * base[2]) > 0.65f,
               "exploded shards should retain clear radial air gaps");
    }
    fracture.fractureMorphology = glasslight::FractureMorphology::Hybrid;
    fracture.fractureMotion = glasslight::FractureMotion::Rigid;
    const auto hybrid = glasslight::buildFractureLayout(fracture);
    const std::uint32_t hybridCore = 6u;
    for (std::uint32_t index = 0; index < hybrid.shardCount; ++index) {
        const auto& base = hybrid.shards[index].base;
        const float radial = std::sqrt(base[0] * base[0] + base[2] * base[2]);
        expect(index < hybridCore ? radial < 0.25f : radial > 0.55f,
               "Hybrid should separate a roughly seventy-percent rooted core from satellites");
    }

    glasslight::CompositionSettings loop;
    loop.rotationRpm = 6.0f;
    expect(std::abs(glasslight::loopDurationSeconds(loop) - 10.0) < 1.0e-9,
           "six RPM should produce an exact ten-second turn");
    loop.rotationRpm = 0.0f;
    expect(!std::isfinite(glasslight::loopDurationSeconds(loop)),
           "a stationary composition should not claim a loop duration");

    // Exercise the actual last-session save/load path without touching the
    // user's config. XDG_CONFIG_HOME is also honored as a test override by the
    // Windows implementation.
    const char* previousConfig = std::getenv("XDG_CONFIG_HOME");
    const std::string previousConfigValue = previousConfig ? previousConfig : "";
    const bool hadPreviousConfig = previousConfig != nullptr;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path testConfigRoot =
        std::filesystem::temp_directory_path() /
        ("glasslight-composition-tests-" + std::to_string(unique));
#if defined(_WIN32)
    _putenv_s("XDG_CONFIG_HOME", testConfigRoot.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", testConfigRoot.string().c_str(), 1);
#endif
    glasslight::CompositionSettings persisted = settings;
    persisted.seed = 0xfacade020ULL;
    persisted.shapeFamily = glasslight::ShapeFamily::CutCrystal;
    persisted.wall = glasslight::WallPreset::Charcoal;
    expect(glasslight::saveLastSession(persisted, error),
           "last-session settings should save atomically: " + error);
    glasslight::CompositionSettings loaded;
    bool found = false;
    expect(glasslight::loadLastSession(loaded, found, error),
           "last-session settings should load: " + error);
    expect(found, "saved last-session settings should be reported as present");
    expect(glasslight::serializeSettings(loaded) ==
               glasslight::serializeSettings(persisted),
           "last-session save/load should preserve the canonical composition");
    std::error_code cleanupError;
    std::filesystem::remove_all(testConfigRoot, cleanupError);
#if defined(_WIN32)
    _putenv_s("XDG_CONFIG_HOME", hadPreviousConfig ? previousConfigValue.c_str() : "");
#else
    if (hadPreviousConfig) {
        setenv("XDG_CONFIG_HOME", previousConfigValue.c_str(), 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
#endif

    if (failures == 0) {
        std::cout << "Composition tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
