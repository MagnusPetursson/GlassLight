#include "media/video_export.hpp"
#include "platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::size_t findArgument(const std::vector<std::string>& arguments,
                         const std::string& argument) {
    const auto found = std::find(arguments.begin(), arguments.end(), argument);
    return found == arguments.end()
               ? arguments.size()
               : static_cast<std::size_t>(found - arguments.begin());
}

} // namespace

int main() {
    using glasslight::media::VideoExportConfig;

    expect(glasslight::media::base64UrlEncode("") == "", "empty base64url");
    expect(glasslight::media::base64UrlEncode("f") == "Zg", "one-byte base64url");
    expect(glasslight::media::base64UrlEncode("fo") == "Zm8", "two-byte base64url");
    expect(glasslight::media::base64UrlEncode("foo") == "Zm9v", "three-byte base64url");
    expect(glasslight::media::base64UrlEncode("\xfb\xff") == "-_8",
           "base64url should use URL-safe alphabet without padding");
    expect(glasslight::media::makeMp4Comment("{}") == "GlassLight/v1:e30",
           "MP4 comment should have a versioned payload prefix");

    VideoExportConfig config;
    config.ffmpegPath = "/opt/FFmpeg Build/bin/ffmpeg";
    config.outputPath = "/tmp/Glass Light/output loop.mp4";
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;
    config.frameCount = 300;
    config.settingsJson = R"({"schemaVersion":1,"seed":42})";

    const std::vector<std::string> arguments =
        glasslight::media::buildFfmpegArguments(config);
    expect(!arguments.empty() && arguments.front() ==
               glasslight::platform::pathToUtf8(config.ffmpegPath),
           "first argv element should be the configured executable");
    expect(arguments.back() == glasslight::platform::pathToUtf8(config.outputPath),
           "output path with spaces should remain one argv element");
    expect(findArgument(arguments, "-f") + 1u < arguments.size() &&
               arguments[findArgument(arguments, "-f") + 1u] == "rawvideo",
           "input format should be rawvideo");
    expect(findArgument(arguments, "-pixel_format") + 1u < arguments.size() &&
               arguments[findArgument(arguments, "-pixel_format") + 1u] == "rgba",
           "input pixel format should be RGBA");
    expect(findArgument(arguments, "-video_size") + 1u < arguments.size() &&
               arguments[findArgument(arguments, "-video_size") + 1u] == "1920x1080",
           "video dimensions should be deterministic");
    expect(findArgument(arguments, "libx264") < arguments.size(),
           "encoder should be libx264");
    expect(findArgument(arguments, "18") < arguments.size(), "CRF should be 18");
    expect(findArgument(arguments, "yuv420p") < arguments.size(),
           "output should use yuv420p");
    expect(findArgument(arguments, "+faststart") < arguments.size(),
           "MP4 should request fast-start layout");
    const std::size_t metadata = findArgument(arguments, "-metadata");
    expect(metadata + 1u < arguments.size() &&
               arguments[metadata + 1u].starts_with("comment=GlassLight/v1:"),
           "settings should be passed through versioned MP4 comment metadata");

    const std::filesystem::path definitelyMissing =
        std::filesystem::temp_directory_path() / "glasslight-no-such-ffmpeg";
    expect(!glasslight::media::findFfmpeg(definitelyMissing).has_value(),
           "explicit missing FFmpeg path should be rejected");

    VideoExportConfig missingConfig;
    missingConfig.outputPath = std::filesystem::temp_directory_path() / "unused.mp4";
    missingConfig.ffmpegPath = definitelyMissing;
    std::string error;
    bool providerCalled = false;
    const bool exported = glasslight::media::exportMp4(
        missingConfig,
        [&](std::uint32_t, std::vector<std::uint8_t>&, std::string&) {
            providerCalled = true;
            return false;
        },
        {}, error);
    expect(!exported, "video export should fail when FFmpeg is missing");
    expect(!providerCalled, "missing FFmpeg should be detected before rendering frames");
    expect(error.find("unavailable") != std::string::npos,
           "missing explicit FFmpeg should produce an actionable error");

    // Exercise the process pipes when FFmpeg is available, while keeping the
    // unit test usable on systems that intentionally omit video support.
    if (const auto ffmpeg = glasslight::media::findFfmpeg()) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path outputDirectory =
            std::filesystem::temp_directory_path() /
            glasslight::platform::pathFromUtf8(
                "glasslight-video-test-\xc3\x9e\xc3\xb3r-" + std::to_string(unique));
        std::filesystem::create_directories(outputDirectory);
        VideoExportConfig tiny;
        tiny.ffmpegPath = *ffmpeg;
        tiny.outputPath = outputDirectory /
            glasslight::platform::pathFromUtf8("seamless-\xe5\x85\x89.mp4");
        tiny.width = 2;
        tiny.height = 2;
        tiny.fps = 2;
        tiny.frameCount = 2;
        tiny.settingsJson = R"({"schemaVersion":1,"seed":7})";
        std::uint32_t completed = 0;
        error.clear();
        expect(glasslight::media::exportMp4(
                   tiny,
                   [](std::uint32_t index, std::vector<std::uint8_t>& rgba,
                      std::string&) {
                       rgba.assign(16u, 255u);
                       rgba[index == 0u ? 1u : 2u] = 32u;
                       return true;
                   },
                   {[&](const glasslight::media::VideoExportProgress& progress) {
                        completed = progress.completedFrames;
                    }, {}},
                   error),
               "tiny MP4 should export through FFmpeg: " + error);
        std::error_code fileError;
        expect(std::filesystem::file_size(tiny.outputPath, fileError) > 0u && !fileError,
               "tiny MP4 should be nonempty");
        expect(completed == tiny.frameCount, "progress should report every encoded frame");
        error.clear();
        expect(glasslight::media::exportMp4(
                   tiny,
                   [](std::uint32_t, std::vector<std::uint8_t>& rgba, std::string&) {
                       rgba.assign(16u, 128u);
                       return true;
                   },
                   {}, error),
               "existing MP4 should be atomically replaceable: " + error);
        std::filesystem::remove_all(outputDirectory, fileError);
    }

#if defined(_WIN32)
    if (const char* basePath = SDL_GetBasePath(); basePath != nullptr) {
        const std::filesystem::path adjacent =
            glasslight::platform::pathFromUtf8(basePath) / "ffmpeg.exe";
        std::error_code fileError;
        const bool alreadyPresent = std::filesystem::exists(adjacent, fileError);
        if (!alreadyPresent && !fileError) {
            std::ofstream placeholder(adjacent, std::ios::binary);
            placeholder.put('\0');
            placeholder.close();
            const auto discovered = glasslight::media::findFfmpeg();
            expect(discovered && std::filesystem::equivalent(
                       *discovered, adjacent, fileError),
                   "Windows should discover ffmpeg.exe beside the application");
            std::filesystem::remove(adjacent, fileError);
        }
    }
#endif

    if (failures == 0) {
        std::cout << "Video export configuration tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
