#include "core/fracture_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glasslight {
namespace {

using Vec3 = std::array<float, 3>;
constexpr float kPi = 3.14159265358979323846f;

Vec3 add(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 multiply(const Vec3& value, const float scale) {
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

float length(const Vec3& value) {
    return std::sqrt(std::max(dot(value, value), 0.0f));
}

Vec3 normalize(const Vec3& value, const Vec3& fallback = {0.0f, 1.0f, 0.0f}) {
    const float magnitude = length(value);
    return magnitude > 1.0e-7f ? multiply(value, 1.0f / magnitude) : fallback;
}

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

struct RandomStream {
    std::uint64_t state;

    std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ULL;
        return splitMix64(state);
    }

    float unit() {
        return static_cast<float>((next() >> 40U) * (1.0 / 16777216.0));
    }

    float range(const float low, const float high) {
        return low + (high - low) * unit();
    }
};

Vec3 radialDirection(const float angle, const float vertical = 0.0f) {
    return normalize({std::cos(angle), vertical, std::sin(angle)});
}

void addPlane(FractureShard& shard, const Vec3& normal, const float offset) {
    if (shard.planeCount >= shard.planes.size()) return;
    const float magnitude = length(normal);
    if (magnitude <= 1.0e-7f) return;
    shard.planes[shard.planeCount++] = {multiply(normal, 1.0f / magnitude),
                                       offset / magnitude};
}

void buildPlanes(FractureShard& shard) {
    shard.planeCount = 0;
    const Vec3 axis = normalize(shard.axis);
    Vec3 u = normalize(subtract(shard.basisU, multiply(axis, dot(shard.basisU, axis))),
                       {1.0f, 0.0f, 0.0f});
    if (std::abs(dot(u, axis)) > 0.99f) {
        u = normalize(cross(axis, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f});
    }
    const Vec3 v = normalize(cross(axis, u), {0.0f, 0.0f, 1.0f});
    shard.axis = axis;
    shard.basisU = u;

    const float shoulder = shard.length * (1.0f - shard.tipFraction);
    const float tipHeight = std::max(shard.length - shoulder, 0.02f);
    const float slope = shard.radius / tipHeight;
    for (std::uint32_t side = 0; side < shard.polygonSides; ++side) {
        const float angle = 2.0f * kPi * static_cast<float>(side) /
                            static_cast<float>(shard.polygonSides);
        const Vec3 radial = add(multiply(u, std::cos(angle)), multiply(v, std::sin(angle)));
        addPlane(shard, radial, dot(radial, shard.base) + shard.radius);
        const Vec3 tipNormal = add(radial, multiply(axis, slope));
        addPlane(shard, tipNormal,
                 dot(radial, shard.base) + shard.radius +
                 slope * (dot(axis, shard.base) + shoulder));
    }
    addPlane(shard, multiply(axis, -1.0f), -dot(axis, shard.base));
}

void assignProfile(FractureShard& shard, const FractureProfile profile,
                   RandomStream& random, const std::uint32_t index) {
    shard.profile = profile;
    switch (profile) {
    case FractureProfile::QuartzPoint:
        shard.polygonSides = 6;
        shard.radius = random.range(0.145f, 0.19f);
        shard.length = random.range(1.25f, 1.48f);
        shard.tipFraction = random.range(0.34f, 0.43f);
        break;
    case FractureProfile::HexagonalColumn:
        shard.polygonSides = 6;
        shard.radius = random.range(0.10f, 0.16f);
        shard.length = random.range(0.72f, 1.12f);
        shard.tipFraction = random.range(0.25f, 0.38f);
        break;
    case FractureProfile::StoutCrystal:
        shard.polygonSides = 4u + static_cast<std::uint32_t>(random.next() % 3u);
        shard.radius = random.range(0.14f, 0.22f);
        shard.length = random.range(0.48f, 0.78f);
        shard.tipFraction = random.range(0.26f, 0.40f);
        break;
    case FractureProfile::Blade:
        shard.polygonSides = 3u + static_cast<std::uint32_t>(random.next() % 2u);
        shard.radius = random.range(0.065f, 0.105f);
        shard.length = random.range(0.78f, 1.20f);
        shard.tipFraction = random.range(0.38f, 0.52f);
        break;
    }
    if (index == 0u) {
        shard.length = std::max(shard.length, 1.32f);
        shard.tipFraction = std::max(shard.tipFraction, 0.36f);
    }
}

void placeGrowthShard(FractureShard& shard, RandomStream& random,
                      const std::uint32_t index, const std::uint32_t count) {
    const float angle = random.range(0.0f, 2.0f * kPi) +
        2.39996323f * static_cast<float>(index);
    const float spread = index == 0u ? 0.0f : random.range(0.06f, 0.18f);
    shard.base = {std::cos(angle) * spread,
                  -0.68f + random.range(-0.025f, 0.035f),
                  std::sin(angle) * spread};
    const float tilt = index == 0u ? random.range(0.02f, 0.10f) :
                                      random.range(0.12f, 0.42f);
    shard.axis = normalize({std::cos(angle) * tilt, 1.0f,
                            std::sin(angle) * tilt});
    shard.separationDirection = radialDirection(angle, random.range(-0.1f, 0.18f));
    (void)count;
}

void placeSatellite(FractureShard& shard, RandomStream& random,
                    const std::uint32_t index, const std::uint32_t count,
                    const float minimumRadius) {
    const float angle = 2.0f * kPi * (static_cast<float>(index) + random.range(0.0f, 0.35f)) /
                        static_cast<float>(std::max(count, 1u));
    const float ring = minimumRadius + 0.16f * static_cast<float>(index % 3u) +
                       random.range(0.0f, 0.10f);
    const float elevation = random.range(-0.30f, 0.26f);
    shard.base = {std::cos(angle) * ring, elevation - shard.length * 0.34f,
                  std::sin(angle) * ring};
    const Vec3 outward = normalize({std::cos(angle), random.range(0.25f, 0.85f),
                                    std::sin(angle)});
    shard.axis = outward;
    shard.separationDirection = normalize({std::cos(angle), random.range(-0.22f, 0.32f),
                                           std::sin(angle)});
}

void computeBounds(FractureLayout& layout) {
    Vec3 minimum{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max()}};
    Vec3 maximum{{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                  -std::numeric_limits<float>::max()}};
    for (std::uint32_t index = 0; index < layout.shardCount; ++index) {
        const FractureShard& shard = layout.shards[index];
        const float circumradius = shard.radius /
            std::cos(kPi / static_cast<float>(shard.polygonSides));
        const Vec3 tip = add(shard.base, multiply(shard.axis, shard.length));
        for (const Vec3& point : {shard.base, tip}) {
            for (std::size_t component = 0; component < 3; ++component) {
                minimum[component] = std::min(minimum[component],
                                              point[component] - circumradius);
                maximum[component] = std::max(maximum[component],
                                              point[component] + circumradius);
            }
        }
    }
    layout.boundsCenter = multiply(add(minimum, maximum), 0.5f);
    layout.boundsRadius = 0.05f;
    for (std::uint32_t index = 0; index < layout.shardCount; ++index) {
        const FractureShard& shard = layout.shards[index];
        const float circumradius = shard.radius /
            std::cos(kPi / static_cast<float>(shard.polygonSides));
        const Vec3 tip = add(shard.base, multiply(shard.axis, shard.length));
        layout.boundsRadius = std::max(layout.boundsRadius,
            length(subtract(shard.base, layout.boundsCenter)) + circumradius);
        layout.boundsRadius = std::max(layout.boundsRadius,
            length(subtract(tip, layout.boundsCenter)) + circumradius);
    }
    layout.boundsRadius += 0.015f;
}

} // namespace

FractureLayout buildFractureLayout(const CompositionSettings& settings) {
    FractureLayout layout;
    layout.shardCount = std::clamp(settings.fractureShardCount, 3u, 16u);
    layout.morphology = resolveFractureMorphology(settings);
    layout.motion = resolveFractureMotion(settings);
    RandomStream random{settings.seed ^ 0x4652414354555245ULL};
    const std::uint32_t coreCount = layout.morphology == FractureMorphology::Hybrid
        ? std::clamp(static_cast<std::uint32_t>(std::lround(layout.shardCount * 0.70f)),
                     2u, layout.shardCount - 1u)
        : (layout.morphology == FractureMorphology::Growth ? layout.shardCount : 0u);
    float wrappedPhase = settings.phase - std::floor(settings.phase);
    if (!std::isfinite(wrappedPhase)) wrappedPhase = 0.0f;
    const float separationEnvelope = std::sin(kPi * wrappedPhase);
    const float separationAmount = layout.motion == FractureMotion::BreakAndReform
        ? 0.45f * separationEnvelope * separationEnvelope : 0.0f;

    for (std::uint32_t index = 0; index < layout.shardCount; ++index) {
        FractureShard& shard = layout.shards[index];
        FractureProfile profile;
        if (index == 0u) profile = FractureProfile::QuartzPoint;
        else if (index == 1u) profile = FractureProfile::HexagonalColumn;
        else if (index == 2u) profile = FractureProfile::Blade;
        else if (index == 3u) profile = FractureProfile::StoutCrystal;
        else profile = static_cast<FractureProfile>(random.next() % 4u);
        assignProfile(shard, profile, random, index);
        shard.primaryPalette = static_cast<std::uint32_t>(random.next() % 5u);
        shard.secondaryPalette = static_cast<std::uint32_t>(random.next() % 5u);
        if (shard.secondaryPalette == shard.primaryPalette) {
            shard.secondaryPalette = (shard.secondaryPalette + 1u + index % 4u) % 5u;
        }

        if (index < coreCount) {
            placeGrowthShard(shard, random, index, coreCount);
        } else {
            const std::uint32_t satelliteIndex = index - coreCount;
            const std::uint32_t satelliteCount = layout.shardCount - coreCount;
            placeSatellite(shard, random, satelliteIndex, satelliteCount,
                           layout.morphology == FractureMorphology::Exploded ? 0.58f : 0.66f);
            if (layout.morphology == FractureMorphology::Exploded) {
                shard.radius *= 0.82f;
                shard.length *= 0.82f;
            } else {
                shard.radius *= 0.88f;
                shard.length *= 0.88f;
            }
        }

        if (layout.motion == FractureMotion::GrowAndRecede) {
            const float offset = static_cast<float>(random.next() & 0xffffu) / 65536.0f;
            const float wave = 0.5f - 0.5f * std::cos(2.0f * kPi * (wrappedPhase + offset));
            shard.length *= 0.70f + 0.30f * wave;
        }
        shard.base = add(shard.base, multiply(shard.separationDirection,
                         separationAmount * (0.72f + 0.28f * random.unit())));

        Vec3 reference = std::abs(shard.axis[1]) < 0.92f ? Vec3{0.0f, 1.0f, 0.0f} :
                                                           Vec3{1.0f, 0.0f, 0.0f};
        shard.basisU = normalize(cross(reference, shard.axis), {1.0f, 0.0f, 0.0f});
        buildPlanes(shard);
    }
    computeBounds(layout);
    return layout;
}

} // namespace glasslight
