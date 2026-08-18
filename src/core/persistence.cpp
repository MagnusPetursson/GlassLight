#include "core/persistence.hpp"
#include "platform/platform.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

namespace glasslight {
namespace {

std::filesystem::path temporarySibling(const std::filesystem::path& destination) {
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return destination.parent_path() / platform::pathFromUtf8(
        platform::pathToUtf8(destination.filename()) + ".tmp." +
        std::to_string(nonce));
}

} // namespace

std::filesystem::path lastSessionPath() {
    return platform::configDirectory() / "settings.json";
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
        error = "Could not open last-session settings at " +
                platform::pathToUtf8(path) + ".";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        error = "Could not read last-session settings at " +
                platform::pathToUtf8(path) + ".";
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
    if (!platform::durableWriteExclusive(
            temporary, serializeSettings(settings), error)) {
        std::filesystem::remove(temporary, fsError);
        return false;
    }

    std::string replaceError;
    if (!platform::replaceFile(temporary, destination, replaceError)) {
        std::filesystem::remove(temporary, fsError);
        error = "Could not replace last-session settings at " +
                platform::pathToUtf8(destination) + ": " + replaceError;
        return false;
    }
    return true;
}

} // namespace glasslight
