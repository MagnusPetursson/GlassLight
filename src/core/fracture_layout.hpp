#pragma once

#include "core/composition.hpp"

#include <array>
#include <cstdint>

namespace glasslight {

constexpr std::uint32_t kMaximumFractureShards = 16;
constexpr std::uint32_t kMaximumFracturePlanes = 13;

enum class FractureProfile : std::uint32_t {
    QuartzPoint = 0,
    HexagonalColumn = 1,
    StoutCrystal = 2,
    Blade = 3
};

struct FracturePlane {
    std::array<float, 3> normal{};
    float offset = 0.0f;
};

struct FractureShard {
    std::array<float, 3> base{};
    std::array<float, 3> axis{{0.0f, 1.0f, 0.0f}};
    std::array<float, 3> basisU{{1.0f, 0.0f, 0.0f}};
    std::array<float, 3> separationDirection{};
    float radius = 0.15f;
    float length = 1.0f;
    float tipFraction = 0.3f;
    std::uint32_t polygonSides = 6;
    std::uint32_t primaryPalette = 0;
    std::uint32_t secondaryPalette = 1;
    FractureProfile profile = FractureProfile::HexagonalColumn;
    std::uint32_t planeCount = 0;
    std::array<FracturePlane, kMaximumFracturePlanes> planes{};
};

struct FractureLayout {
    std::uint32_t shardCount = 0;
    FractureMorphology morphology = FractureMorphology::Growth;
    FractureMotion motion = FractureMotion::Rigid;
    std::array<FractureShard, kMaximumFractureShards> shards{};
    std::array<float, 3> boundsCenter{};
    float boundsRadius = 1.0f;
};

FractureLayout buildFractureLayout(const CompositionSettings& settings);

} // namespace glasslight
