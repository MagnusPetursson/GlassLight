#include "render/adaptive_controller.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace glasslight {
namespace {

constexpr double kIdleDelayMilliseconds = 250.0;
constexpr double kNoticeDelayMilliseconds = 2'000.0;
constexpr double kRecoveryDelayMilliseconds = 5'000.0;
constexpr std::size_t kTimingWindow = 120;

double percentile95(const std::deque<double>& values) {
    if (values.empty()) return 0.0;
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(static_cast<double>(sorted.size()) * 0.95)) - 1u;
    return sorted[std::min(index, sorted.size() - 1u)];
}

} // namespace

void AdaptiveRenderController::reset(const QualityPreset ceiling,
                                     const std::uint32_t width,
                                     const std::uint32_t height,
                                     const double nowMilliseconds) {
    ceiling_ = ceiling;
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    photons_ = floorPhotons();
    interfaceDepth_ = QualityPreset::Draft;
    previewScale_ = 0.5f;
    paused_ = false;
    notice_ = false;
    underBudgetFrames_ = 0;
    previewUnderBudgetFrames_ = 0;
    lastInteractionMilliseconds_ = nowMilliseconds;
    traceMillisecondsPerPhoton_ = 0.0;
    fixedMilliseconds_ = 0.0;
    minimumFailureSince_ = -1.0;
    recoverySince_ = -1.0;
    frameIntervals_.clear();
}

void AdaptiveRenderController::noteInteraction(const double nowMilliseconds) {
    lastInteractionMilliseconds_ = nowMilliseconds;
    photons_ = floorPhotons();
    interfaceDepth_ = QualityPreset::Draft;
    previewScale_ = 0.5f;
    underBudgetFrames_ = 0;
    previewUnderBudgetFrames_ = 0;
}

void AdaptiveRenderController::setPaused(const bool paused,
                                         const double nowMilliseconds) {
    paused_ = paused;
    if (paused) {
        interfaceDepth_ = ceiling_;
        previewScale_ = 1.0f;
    } else {
        noteInteraction(nowMilliseconds);
    }
}

void AdaptiveRenderController::update(const RenderTimings& timings,
                                      const double nowMilliseconds) {
    const double trace = std::max(timings.traceMilliseconds, 0.0);
    const double fixed = std::max(0.0, timings.resolveMilliseconds) +
                         std::max(0.0, timings.transferMilliseconds) +
                         std::max(0.0, timings.sdlUploadMilliseconds);
    if (trace > 0.0 && photons_ > 0u) {
        const double observed = trace / static_cast<double>(photons_);
        traceMillisecondsPerPhoton_ = traceMillisecondsPerPhoton_ == 0.0
            ? observed : traceMillisecondsPerPhoton_ * 0.75 + observed * 0.25;
    }
    fixedMilliseconds_ = fixedMilliseconds_ == 0.0
        ? fixed : fixedMilliseconds_ * 0.75 + fixed * 0.25;

    if (timings.completedFrameIntervalMilliseconds > 0.0) {
        frameIntervals_.push_back(timings.completedFrameIntervalMilliseconds);
        while (frameIntervals_.size() > kTimingWindow) frameIntervals_.pop_front();
    }

    const std::uint32_t floor = floorPhotons();
    const std::uint32_t ceiling = ceilingPhotons();
    const double availableTrace = std::max(0.0,
        kFrameBudgetMilliseconds - fixedMilliseconds_);
    const double projected = fixedMilliseconds_ +
        traceMillisecondsPerPhoton_ * static_cast<double>(photons_);

    if (traceMillisecondsPerPhoton_ > 0.0 && projected > kFrameBudgetMilliseconds) {
        const std::uint32_t target = quantize(
            availableTrace / traceMillisecondsPerPhoton_);
        photons_ = std::clamp(std::min(photons_, target), floor, ceiling);
        underBudgetFrames_ = 0;
        if (photons_ == floor) interfaceDepth_ = QualityPreset::Draft;
    } else if (projected <= kFrameBudgetMilliseconds) {
        ++underBudgetFrames_;
        if (underBudgetFrames_ >= 8u) {
            const std::uint32_t raised = quantize(
                static_cast<double>(photons_) * 1.10);
            photons_ = std::clamp(std::max(raised, photons_ + 256u), floor, ceiling);
            underBudgetFrames_ = 0;
            const bool sustainedHeadroom = projected <= kFrameBudgetMilliseconds * 0.82;
            if (sustainedHeadroom && photons_ > floor) interfaceDepth_ = ceiling_;
        }
    }

    const bool refining = paused_ ||
        nowMilliseconds - lastInteractionMilliseconds_ >= kIdleDelayMilliseconds;
    if (refining) interfaceDepth_ = ceiling_;
    if (refining && photons_ < ceiling) {
        // Refinement is still bounded to 10% per accepted eight-frame step;
        // reset() guarantees a materially changed composition starts at Draft.
        previewScale_ = photons_ >= ceiling / 2u ? 0.75f : 0.5f;
    } else if (refining) {
        interfaceDepth_ = ceiling_;
        previewScale_ = 1.0f;
    } else {
        const double total = timings.submitToReadyMilliseconds +
                             timings.sdlUploadMilliseconds;
        if (total > kFrameBudgetMilliseconds * 1.08) {
            previewUnderBudgetFrames_ = 0;
            previewScale_ = previewScale_ > 0.75f ? 0.75f : 0.5f;
        } else if (total < kFrameBudgetMilliseconds * 0.72 && photons_ > floor) {
            ++previewUnderBudgetFrames_;
            if (previewUnderBudgetFrames_ >= 8u) {
                previewScale_ = previewScale_ < 0.75f ? 0.75f : 1.0f;
                previewUnderBudgetFrames_ = 0;
            }
        } else {
            previewUnderBudgetFrames_ = 0;
        }
    }
    updateNotice(nowMilliseconds);
}

InteractiveWorkload AdaptiveRenderController::workload(
    const double nowMilliseconds) const {
    InteractiveWorkload result;
    result.photons = photons_;
    result.interfaceDepth = interfaceDepth_;
    result.glassPreviewScale = previewScale_;
    result.refining = paused_ ||
        nowMilliseconds - lastInteractionMilliseconds_ >= kIdleDelayMilliseconds;
    if (result.refining) {
        result.interfaceDepth = ceiling_;
        result.glassPreviewScale = 1.0f;
    }
    result.minimumWorkload = photons_ <= floorPhotons() &&
                             interfaceDepth_ == QualityPreset::Draft;
    result.performanceNotice = notice_;
    result.refinementProgress = std::clamp(
        static_cast<float>(photons_) / static_cast<float>(ceilingPhotons()), 0.0f, 1.0f);
    if (!frameIntervals_.empty()) {
        double total = 0.0;
        for (const double interval : frameIntervals_) total += interval;
        result.deliveredFps = 1000.0 * static_cast<double>(frameIntervals_.size()) / total;
        result.p95FrameMilliseconds = percentile95(frameIntervals_);
    }
    return result;
}

std::uint32_t AdaptiveRenderController::floorPhotons() const {
    const double megapixels = static_cast<double>(width_) * height_ / 1'000'000.0;
    return quantize(photonsPerMegapixel(QualityPreset::Draft) * megapixels);
}

std::uint32_t AdaptiveRenderController::ceilingPhotons() const {
    const double megapixels = static_cast<double>(width_) * height_ / 1'000'000.0;
    return std::max(floorPhotons(), quantize(photonsPerMegapixel(ceiling_) * megapixels));
}

std::uint32_t AdaptiveRenderController::quantize(const double photons) {
    const double safe = std::clamp(photons, 256.0,
        static_cast<double>(0xffff'ff00u));
    return static_cast<std::uint32_t>(safe / 256.0) * 256u;
}

void AdaptiveRenderController::updateNotice(const double nowMilliseconds) {
    const InteractiveWorkload current = workload(nowMilliseconds);
    const bool failing = current.minimumWorkload && frameIntervals_.size() >= 8u &&
        (current.deliveredFps < 28.0 || current.p95FrameMilliseconds > 40.0);
    if (failing) {
        recoverySince_ = -1.0;
        if (minimumFailureSince_ < 0.0) minimumFailureSince_ = nowMilliseconds;
        if (nowMilliseconds - minimumFailureSince_ >= kNoticeDelayMilliseconds) {
            notice_ = true;
        }
    } else {
        minimumFailureSince_ = -1.0;
        if (notice_) {
            if (recoverySince_ < 0.0) recoverySince_ = nowMilliseconds;
            if (nowMilliseconds - recoverySince_ >= kRecoveryDelayMilliseconds) {
                notice_ = false;
                recoverySince_ = -1.0;
            }
        }
    }
}

} // namespace glasslight
