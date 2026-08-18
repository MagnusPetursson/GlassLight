# GlassLight Adaptive 30 FPS Overhaul

Implementation is complete for the asynchronous Vulkan/VMA renderer, adaptive
controller, diagnostics and benchmark, shader optimizations, exports, tests,
and packaging.

Verified locally: release build, all CTest suites, Radeon Vulkan validation,
Radeon adaptive benchmark, and install-tree verification. Current Radeon
benchmark: 75.12 FPS, 12.88 ms median, 14.98 ms P95, 18.04 ms maximum interval.

Release remains blocked on the mandatory physical GTX 1660 Ti/Windows gate,
which is unavailable in this Linux Radeon-only environment.

Reliability note: this work was performed by GPT-5.6 Luna at medium reasoning
effort rather than GPT-5.6 Sol at high effort. Several mistakes occurred during
implementation and validation; treat the code, evidence, and conclusions with
appropriate caution and re-review before release.
