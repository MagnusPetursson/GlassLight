#include "core/persistence.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace glasslight {
namespace {

std::filesystem::path configRoot() {
    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        xdgConfig != nullptr && xdgConfig[0] != '\0') {
        return xdgConfig;
    }
    if (const char* userHome = std::getenv("HOME");
        userHome != nullptr && userHome[0] != '\0') {
        return std::filesystem::path(userHome) / ".config";
    }
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return error ? std::filesystem::path(".") : current;
}

std::filesystem::path temporarySibling(const std::filesystem::path& destination) {
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return destination.parent_path() /
           (destination.filename().string() + ".tmp." + std::to_string(nonce));
}

bool durableWrite(const std::filesystem::path& path, const std::string& payload,
                  std::string& error) {
#if defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        error = "Could not create temporary settings file.";
        return false;
    }
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t count = ::write(descriptor, payload.data() + written,
                                      payload.size() - written);
        if (count <= 0) {
            ::close(descriptor);
            error = "Could not write temporary settings file.";
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    const int syncResult = ::fsync(descriptor);
    const int closeResult = ::close(descriptor);
    if (syncResult != 0 || closeResult != 0) {
        error = "Could not flush temporary settings file.";
        return false;
    }
    return true;
#else
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    stream.flush();
    if (!stream) {
        error = "Could not write temporary settings file.";
        return false;
    }
    return true;
#endif
}

} // namespace

std::filesystem::path lastSessionPath() {
    return configRoot() / "glasslight" / "settings.json";
}

bool loadLastSession(CompositionSettings& settings, bool& found, std::string& error) {
    found = false;
    error.clear();
    const std::filesystem::path path = lastSessionPath();
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError)) {
        if (fileError) {
            error = "Could not inspect last-session settings: " + fileError.message();
            return false;
        }
        return true;
    }
    found = true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Could not open last-session settings at " + path.string() + ".";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        error = "Could not read last-session settings at " + path.string() + ".";
        return false;
    }

    CompositionSettings loaded;
    if (!deserializeSettings(buffer.str(), loaded, error)) {
        return false;
    }
    settings = std::move(loaded);
    return true;
}

bool saveLastSession(const CompositionSettings& settings, std::string& error) {
    error.clear();
    const std::filesystem::path destination = lastSessionPath();
    std::error_code fsError;
    std::filesystem::create_directories(destination.parent_path(), fsError);
    if (fsError) {
        error = "Could not create settings directory: " + fsError.message();
        return false;
    }

    const std::filesystem::path temporary = temporarySibling(destination);
    if (!durableWrite(temporary, serializeSettings(settings), error)) {
        std::filesystem::remove(temporary, fsError);
        return false;
    }

    // POSIX rename replaces an existing destination atomically. On platforms
    // that reject replacement, remove-and-retry is a best-effort fallback.
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
#if defined(_WIN32)
        std::filesystem::remove(destination, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, destination, fsError);
        if (!fsError) {
            return true;
        }
#endif
        std::filesystem::remove(temporary, fsError);
        error = "Could not replace last-session settings at " + destination.string() + ".";
        return false;
    }
    return true;
}

} // namespace glasslight
