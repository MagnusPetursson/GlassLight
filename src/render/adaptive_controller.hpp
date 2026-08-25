#pragma once

#include "core/composition.hpp"

#include <cstdint>
#include <deque>

namespace glasslight {

struct RenderTimings {
    double traceMilliseconds = 0.0;
    double resolveMilliseconds = 0.0;
    double transferMilliseconds = 0.0;
    double submitToReadyMilliseconds = 0.0;
    double sdlUploadMilliseconds = 0.0;
    double completedFrameIntervalMilliseconds = 0.0;
};

struct InteractiveWorkload {
    std::uint32_t photons = 256;
    QualityPreset interfaceDepth = QualityPreset::Draft;
    float glassPreviewScale = 0.5f;
    bool refining = false;
    bool minimumWorkload = true;
    bool performanceNotice = false;
    float refinementProgress = 0.0f;
    double deliveredFps = 0.0;
    double p95FrameMilliseconds = 0.0;
};

// Capability-neutral measured-time controller for interactive work only.
// Time is supplied by the caller so behavior is deterministic in unit tests.
class AdaptiveRenderController {
public:
    static constexpr double kFrameBudgetMilliseconds = 27.0;

    void reset(QualityPreset ceiling, std::uint32_t width,
               std::uint32_t height, double nowMilliseconds = 0.0);
    void noteInteraction(double nowMilliseconds);
    void setPaused(bool paused, double nowMilliseconds);
    void update(const RenderTimings& timings, double nowMilliseconds);
    [[nodiscard]] InteractiveWorkload workload(double nowMilliseconds) const;

private:
    [[nodiscard]] std::uint32_t floorPhotons() const;
    [[nodiscard]] std::uint32_t ceilingPhotons() const;
    [[nodiscard]] static std::uint32_t quantize(double photons);
    void updateNotice(double nowMilliseconds);

    QualityPreset ceiling_ = QualityPreset::Standard;
    std::uint32_t width_ = 1280;
    std::uint32_t height_ = 720;
    std::uint32_t photons_ = 256;
    QualityPreset interfaceDepth_ = QualityPreset::Draft;
    float previewScale_ = 0.5f;
    bool paused_ = false;
    bool notice_ = false;
    std::uint32_t underBudgetFrames_ = 0;
    std::uint32_t previewUnderBudgetFrames_ = 0;
    double lastInteractionMilliseconds_ = 0.0;
    double traceMillisecondsPerPhoton_ = 0.0;
    double fixedMilliseconds_ = 0.0;
    double minimumFailureSince_ = -1.0;
    double recoverySince_ = -1.0;
    std::deque<double> frameIntervals_;
};

} // namespace glasslight
