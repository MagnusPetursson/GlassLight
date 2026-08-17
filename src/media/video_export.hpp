#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace glasslight::media {

struct VideoExportConfig {
    std::filesystem::path outputPath;
    std::filesystem::path ffmpegPath;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t fps = 30;
    std::uint32_t frameCount = 300;
    std::string settingsJson;
    bool overwrite = true;
};

struct VideoExportProgress {
    std::uint32_t completedFrames = 0;
    std::uint32_t totalFrames = 0;
};

struct VideoExportCallbacks {
    std::function<void(const VideoExportProgress&)> progress;
    std::function<bool()> isCancelled;
};

// The provider must return exactly width * height * 4 bytes of row-major RGBA.
using VideoFrameProvider =
    std::function<bool(std::uint32_t frameIndex,
                       std::vector<std::uint8_t>& rgba,
                       std::string& error)>;

std::optional<std::filesystem::path>
findFfmpeg(const std::filesystem::path& preferred = {});

std::string base64UrlEncode(const std::string& payload);
std::string makeMp4Comment(const std::string& settingsJson);

// Exposed so the exact codec/container contract can be unit tested. Each
// element is one argv entry; no shell quoting or command string is involved.
std::vector<std::string> buildFfmpegArguments(const VideoExportConfig& config);

bool exportMp4(const VideoExportConfig& config,
               const VideoFrameProvider& frameProvider,
               const VideoExportCallbacks& callbacks,
               std::string& error);

} // namespace glasslight::media
