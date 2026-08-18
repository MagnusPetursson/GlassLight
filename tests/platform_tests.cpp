#include "platform/platform.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

} // namespace

int main() {
    using glasslight::platform::pathFromUtf8;
    using glasslight::platform::pathToUtf8;

    const std::string unicodeName = "GlassLight-\xc3\x9e\xc3\xb3r-\xe5\x85\x89";
    const std::filesystem::path unicodePath = pathFromUtf8(unicodeName);
    expect(pathToUtf8(unicodePath) == unicodeName,
           "filesystem paths should round-trip through UTF-8");
#if defined(_WIN32)
    expect(pathToUtf8(glasslight::platform::configDirectory().filename()) == "GlassLight",
           "Windows configuration should use an AppData GlassLight directory");
#endif

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        pathFromUtf8(unicodeName + "-" + std::to_string(unique));
    std::error_code filesystemError;
    std::filesystem::create_directories(root, filesystemError);
    expect(!filesystemError, "Unicode temporary directory should be created");

    const std::filesystem::path destination = root / pathFromUtf8("settings-\xc3\xa6.json");
    const std::filesystem::path firstTemporary = root / "first.tmp";
    const std::filesystem::path secondTemporary = root / "second.tmp";
    std::string error;
    expect(glasslight::platform::durableWriteExclusive(firstTemporary, "first", error),
           "first durable write should succeed: " + error);
    expect(glasslight::platform::replaceFile(firstTemporary, destination, error),
           "first replacement should create the destination: " + error);
    expect(readText(destination) == "first", "first replacement should preserve content");

    error.clear();
    expect(glasslight::platform::durableWriteExclusive(secondTemporary, "second", error),
           "second durable write should succeed: " + error);
    expect(glasslight::platform::replaceFile(secondTemporary, destination, error),
           "second replacement should overwrite the destination: " + error);
    expect(readText(destination) == "second", "replacement should update existing content");

    std::filesystem::remove_all(root, filesystemError);
    if (failures == 0) {
        std::cout << "Platform tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
