#include "render/adaptive_controller.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    glasslight::AdaptiveRenderController controller;
    controller.reset(glasslight::QualityPreset::Standard, 1280, 720, 0.0);
    auto work = controller.workload(0.0);
    expect(work.photons == 460'800u, "Draft floor is exact and 256-quantized");
    expect(work.interfaceDepth == glasslight::QualityPreset::Draft,
           "new compositions start at Draft interface depth");
    expect(work.glassPreviewScale == 0.5f, "preview starts at half resolution");

    glasslight::RenderTimings slow;
    slow.traceMilliseconds = 54.0;
    slow.resolveMilliseconds = 3.0;
    slow.transferMilliseconds = 2.0;
    slow.sdlUploadMilliseconds = 2.0;
    slow.submitToReadyMilliseconds = 61.0;
    slow.completedFrameIntervalMilliseconds = 50.0;
    controller.update(slow, 50.0);
    work = controller.workload(50.0);
    expect(work.photons == 460'800u, "controller never falls below Draft density");
    expect(work.interfaceDepth == glasslight::QualityPreset::Draft,
           "depth falls only to Draft");

    controller.reset(glasslight::QualityPreset::Standard, 1280, 720, 60.0);
    glasslight::RenderTimings fast;
    fast.traceMilliseconds = 8.0;
    fast.resolveMilliseconds = 2.0;
    fast.transferMilliseconds = 1.0;
    fast.sdlUploadMilliseconds = 1.0;
    fast.submitToReadyMilliseconds = 12.0;
    fast.completedFrameIntervalMilliseconds = 32.0;
    for (int index = 0; index < 8; ++index) controller.update(fast, 100.0 + index * 32.0);
    work = controller.workload(356.0);
    expect(work.photons > 460'800u && work.photons <= 507'136u,
           "quality rises by at most ten percent after eight good frames");
    expect((work.photons & 255u) == 0u, "workload stays quantized to 256 invocations");

    controller.noteInteraction(400.0);
    expect(!controller.workload(649.0).refining, "idle hysteresis waits 250 ms");
    expect(controller.workload(650.0).refining &&
           controller.workload(650.0).glassPreviewScale == 1.0f,
           "idle hysteresis restores full preview at 250 ms");
    controller.setPaused(true, 651.0);
    expect(controller.workload(651.0).refining, "pause enters refinement immediately");
    expect(controller.workload(651.0).interfaceDepth == glasslight::QualityPreset::Standard &&
           controller.workload(651.0).glassPreviewScale == 1.0f,
           "pause restores selected interface depth and full preview immediately");

    controller.reset(glasslight::QualityPreset::Draft, 1280, 720, 0.0);
    for (int index = 1; index <= 60; ++index) {
        slow.completedFrameIntervalMilliseconds = 50.0;
        controller.update(slow, index * 50.0);
    }
    expect(controller.workload(3'000.0).performanceNotice,
           "notice appears after two seconds at a failing minimum");
    for (int index = 1; index <= 300; ++index) {
        fast.completedFrameIntervalMilliseconds = 32.0;
        controller.update(fast, 3'000.0 + index * 32.0);
    }
    expect(!controller.workload(12'600.0).performanceNotice,
           "notice clears after five seconds of recovery");

    if (failures == 0) std::cout << "Adaptive controller tests passed.\n";
    return failures == 0 ? 0 : 1;
}
