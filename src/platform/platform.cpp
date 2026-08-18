#include "platform/platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace glasslight::platform {

std::filesystem::path pathFromUtf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::string pathToUtf8(const std::filesystem::path& value) {
    const std::u8string encoded = value.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path configDirectory() {
    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        xdgConfig != nullptr && xdgConfig[0] != '\0') {
        return pathFromUtf8(xdgConfig) / "glasslight";
    }

#if defined(_WIN32)
    PWSTR roamingPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &roamingPath))) {
        const std::filesystem::path result =
            std::filesystem::path(roamingPath) / L"GlassLight";
        CoTaskMemFree(roamingPath);
        return result;
    }
    if (const char* appData = std::getenv("APPDATA");
        appData != nullptr && appData[0] != '\0') {
        return pathFromUtf8(appData) / "GlassLight";
    }
#else
    if (const char* userHome = std::getenv("HOME");
        userHome != nullptr && userHome[0] != '\0') {
        return pathFromUtf8(userHome) / ".config" / "glasslight";
    }
#endif

    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return (error ? std::filesystem::path(".") : current) / "glasslight";
}

bool durableWriteExclusive(const std::filesystem::path& path,
                           const std::string_view payload,
                           std::string& error) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Could not create temporary settings file: " +
                std::system_category().message(static_cast<int>(GetLastError()));
        return false;
    }
    std::size_t written = 0;
    while (written < payload.size()) {
        const std::size_t remaining = payload.size() - written;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!WriteFile(file, payload.data() + written, requested, &count, nullptr) ||
            count == 0) {
            error = "Could not write temporary settings file: " +
                    std::system_category().message(static_cast<int>(GetLastError()));
            CloseHandle(file);
            return false;
        }
        written += count;
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    const bool closed = CloseHandle(file) != 0;
    if (!flushed || !closed) {
        error = "Could not flush temporary settings file.";
        return false;
    }
    return true;
#elif defined(__unix__) || defined(__APPLE__)
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

bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string& error) {
#if defined(_WIN32)
    if (ReplaceFileW(destination.c_str(), temporary.c_str(), nullptr,
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != 0) {
        return true;
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    error = std::system_category().message(static_cast<int>(GetLastError()));
    return false;
#elif defined(__unix__) || defined(__APPLE__)
    if (std::rename(temporary.c_str(), destination.c_str()) == 0) {
        return true;
    }
    error = std::system_category().message(errno);
    return false;
#else
    std::error_code filesystemError;
    std::filesystem::rename(temporary, destination, filesystemError);
    if (!filesystemError) {
        return true;
    }
    error = filesystemError.message();
    return false;
#endif
}

} // namespace glasslight::platform
