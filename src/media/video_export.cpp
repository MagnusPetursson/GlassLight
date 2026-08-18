#include "media/video_export.hpp"
#include "platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace glasslight::media {
namespace {

constexpr std::size_t kMaximumMetadataBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaximumErrorBytes = 64u * 1024u;

bool isExecutable(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

std::filesystem::path temporaryMp4Sibling(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return path.parent_path() / platform::pathFromUtf8(
        platform::pathToUtf8(path.stem()) + ".tmp." + std::to_string(tick) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".mp4");
}

void drainErrors(SDL_IOStream* stream, std::string& output) {
    if (!stream) {
        return;
    }
    std::array<char, 4096> buffer{};
    while (true) {
        const std::size_t count = SDL_ReadIO(stream, buffer.data(), buffer.size());
        if (count == 0u) {
            return;
        }
        if (output.size() < kMaximumErrorBytes) {
            const std::size_t remaining = kMaximumErrorBytes - output.size();
            output.append(buffer.data(), std::min(count, remaining));
        }
    }
}

std::string conciseProcessError(std::string output) {
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
    }
    if (output.empty()) {
        return "FFmpeg did not provide an error message.";
    }
    return output;
}

bool waitForProcess(SDL_Process* process,
                    SDL_IOStream* errorStream,
                    int& exitCode,
                    std::string& processError) {
    while (!SDL_WaitProcess(process, false, &exitCode)) {
        drainErrors(errorStream, processError);
        SDL_Delay(5u);
    }
    drainErrors(errorStream, processError);
    return true;
}

void terminateProcess(SDL_Process* process,
                      SDL_IOStream* input,
                      SDL_IOStream* errorStream,
                      std::string& processError) {
    if (input) {
        SDL_CloseIO(input);
    }
    SDL_KillProcess(process, false);
    int exitCode = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        drainErrors(errorStream, processError);
        if (SDL_WaitProcess(process, false, &exitCode)) {
            return;
        }
        SDL_Delay(5u);
    }
    SDL_KillProcess(process, true);
    waitForProcess(process, errorStream, exitCode, processError);
}

bool writeAll(SDL_IOStream* stream,
              const std::uint8_t* data,
              std::size_t size,
              SDL_IOStream* errorStream,
              std::string& processError) {
    std::size_t written = 0;
    int notReadyRetries = 0;
    while (written < size) {
        const std::size_t count = SDL_WriteIO(stream, data + written, size - written);
        drainErrors(errorStream, processError);
        if (count == 0u) {
            // SDL process pipes are non-blocking on POSIX. A full pipe reports
            // NOT_READY while FFmpeg consumes the previous chunk; this is
            // backpressure, not process failure.
            if (SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY &&
                notReadyRetries++ < 30'000) {
                SDL_Delay(1u);
                continue;
            }
            return false;
        }
        notReadyRetries = 0;
        written += count;
    }
    return true;
}

} // namespace

std::optional<std::filesystem::path>
findFfmpeg(const std::filesystem::path& preferred) {
    if (!preferred.empty()) {
        if (isExecutable(preferred)) {
            return std::filesystem::absolute(preferred);
        }
        return std::nullopt;
    }

#if defined(_WIN32)
    constexpr char separator = ';';
    constexpr const char* executableName = "ffmpeg.exe";
    if (const char* basePath = SDL_GetBasePath(); basePath != nullptr) {
        const std::filesystem::path adjacent =
            platform::pathFromUtf8(basePath) / executableName;
        if (isExecutable(adjacent)) {
            return std::filesystem::absolute(adjacent);
        }
    }
#else
    constexpr char separator = ':';
    constexpr const char* executableName = "ffmpeg";
#endif
    const char* pathVariable =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "PATH");
    if (!pathVariable) {
        return std::nullopt;
    }
    const std::string searchPath(pathVariable);
    std::size_t start = 0;
    while (start <= searchPath.size()) {
        const std::size_t end = searchPath.find(separator, start);
        const std::string directory = searchPath.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::filesystem::path candidate =
            (directory.empty() ? std::filesystem::current_path()
                               : platform::pathFromUtf8(directory)) /
            executableName;
        if (isExecutable(candidate)) {
            return std::filesystem::absolute(candidate);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1u;
    }
    return std::nullopt;
}

std::string base64UrlEncode(const std::string& payload) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((payload.size() * 4u + 2u) / 3u);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char byte : payload) {
        accumulator = (accumulator << 8u) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(accumulator >> bits) & 0x3fu]);
        }
    }
    if (bits > 0) {
        output.push_back(alphabet[(accumulator << (6 - bits)) & 0x3fu]);
    }
    return output;
}

std::string makeMp4Comment(const std::string& settingsJson) {
    return "GlassLight/v1:" + base64UrlEncode(settingsJson);
}

std::vector<std::string> buildFfmpegArguments(const VideoExportConfig& config) {
    const std::filesystem::path executable =
        config.ffmpegPath.empty() ? std::filesystem::path("ffmpeg") : config.ffmpegPath;
    return {
        platform::pathToUtf8(executable),
        "-hide_banner",
        "-loglevel", "error",
        "-nostats",
        "-y",
        "-f", "rawvideo",
        "-pixel_format", "rgba",
        "-video_size", std::to_string(config.width) + "x" + std::to_string(config.height),
        "-framerate", std::to_string(config.fps),
        "-i", "pipe:0",
        "-an",
        "-c:v", "libx264",
        "-preset", "medium",
        "-crf", "18",
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-metadata", "comment=" + makeMp4Comment(config.settingsJson),
        platform::pathToUtf8(config.outputPath)
    };
}

bool exportMp4(const VideoExportConfig& config,
               const VideoFrameProvider& frameProvider,
               const VideoExportCallbacks& callbacks,
               std::string& error) {
    error.clear();
    if (!frameProvider) {
        error = "Video export requires a frame provider.";
        return false;
    }
    if (config.width == 0u || config.height == 0u || config.fps == 0u ||
        config.frameCount == 0u || (config.width & 1u) != 0u ||
        (config.height & 1u) != 0u) {
        error = "Video dimensions must be nonzero and even, and FPS/frame count must be nonzero.";
        return false;
    }
    const std::uint64_t frameBytes64 =
        static_cast<std::uint64_t>(config.width) * config.height * 4u;
    if (frameBytes64 > std::numeric_limits<std::size_t>::max()) {
        error = "Video frame dimensions exceed addressable memory.";
        return false;
    }
    if (config.settingsJson.size() > kMaximumMetadataBytes) {
        error = "Video metadata exceeds the 4 MiB safety limit.";
        return false;
    }
    if (config.outputPath.empty()) {
        error = "Video output path is empty.";
        return false;
    }
    std::error_code filesystemError;
    if (!config.overwrite && std::filesystem::exists(config.outputPath, filesystemError)) {
        error = "Video output already exists.";
        return false;
    }
    if (!config.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(config.outputPath.parent_path(), filesystemError);
        if (filesystemError) {
            error = "Could not create video directory: " + filesystemError.message();
            return false;
        }
    }

    const auto ffmpeg = findFfmpeg(config.ffmpegPath);
    if (!ffmpeg) {
        error = config.ffmpegPath.empty()
                    ? "FFmpeg was not found beside GlassLight or on PATH. "
                      "Add ffmpeg.exe beside the app or install FFmpeg to export MP4 video."
                    : "The configured FFmpeg executable is unavailable: " +
                          platform::pathToUtf8(config.ffmpegPath);
        return false;
    }

    const std::filesystem::path temporary = temporaryMp4Sibling(config.outputPath);
    VideoExportConfig processConfig = config;
    processConfig.ffmpegPath = *ffmpeg;
    processConfig.outputPath = temporary;
    const std::vector<std::string> arguments = buildFfmpegArguments(processConfig);
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1u);
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0u) {
        error = "Could not prepare FFmpeg process: " + std::string(SDL_GetError());
        return false;
    }
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!process) {
        error = "Could not start FFmpeg: " + std::string(SDL_GetError());
        return false;
    }

    SDL_IOStream* input = SDL_GetProcessInput(process);
    const SDL_PropertiesID processProperties = SDL_GetProcessProperties(process);
    auto* errorStream = static_cast<SDL_IOStream*>(SDL_GetPointerProperty(
        processProperties, SDL_PROP_PROCESS_STDERR_POINTER, nullptr));
    if (!input || !errorStream) {
        std::string ignoredOutput;
        terminateProcess(process, input, errorStream, ignoredOutput);
        SDL_DestroyProcess(process);
        error = "Could not open FFmpeg process pipes: " + std::string(SDL_GetError());
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }

    std::string processError;
    std::vector<std::uint8_t> frame;
    const std::size_t expectedFrameBytes = static_cast<std::size_t>(frameBytes64);
    for (std::uint32_t index = 0; index < config.frameCount; ++index) {
        if (callbacks.isCancelled && callbacks.isCancelled()) {
            terminateProcess(process, input, errorStream, processError);
            SDL_DestroyProcess(process);
            std::filesystem::remove(temporary, filesystemError);
            error = "Video export cancelled.";
            return false;
        }
        frame.clear();
        if (!frameProvider(index, frame, error)) {
            terminateProcess(process, input, errorStream, processError);
            SDL_DestroyProcess(process);
            std::filesystem::remove(temporary, filesystemError);
            if (error.empty()) {
                error = "Video frame provider failed at frame " + std::to_string(index) + ".";
            }
            return false;
        }
        if (frame.size() != expectedFrameBytes) {
            terminateProcess(process, input, errorStream, processError);
            SDL_DestroyProcess(process);
            std::filesystem::remove(temporary, filesystemError);
            error = "Video frame " + std::to_string(index) +
                    " has the wrong RGBA byte count.";
            return false;
        }
        if (!writeAll(input, frame.data(), frame.size(), errorStream, processError)) {
            terminateProcess(process, input, errorStream, processError);
            SDL_DestroyProcess(process);
            std::filesystem::remove(temporary, filesystemError);
            error = "FFmpeg stopped accepting video frames. " +
                    conciseProcessError(processError);
            return false;
        }
        if (callbacks.progress) {
            callbacks.progress({index + 1u, config.frameCount});
        }
    }

    SDL_CloseIO(input);
    input = nullptr;
    int exitCode = -1;
    waitForProcess(process, errorStream, exitCode, processError);
    SDL_DestroyProcess(process);
    if (exitCode != 0) {
        std::filesystem::remove(temporary, filesystemError);
        error = "FFmpeg failed with exit code " + std::to_string(exitCode) + ". " +
                conciseProcessError(processError);
        return false;
    }

    std::string replaceError;
    if (!platform::replaceFile(temporary, config.outputPath, replaceError)) {
        std::filesystem::remove(temporary, filesystemError);
        error = "Could not replace video output atomically: " + replaceError;
        return false;
    }
    return true;
}

} // namespace glasslight::media
