#include "media/png_metadata.hpp"
#include "core/composition.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
    using glasslight::media::RgbaImage;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        glasslight::platform::pathFromUtf8(
            "glasslight-png-tests-\xc3\x9e\xc3\xb3r-" + std::to_string(unique));
    std::filesystem::create_directories(directory);
    const std::filesystem::path pngPath =
        directory / glasslight::platform::pathFromUtf8("composition-\xe5\x85\x89.png");

    RgbaImage source;
    source.width = 3;
    source.height = 2;
    source.pixels = {
        255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64,
        255, 255, 0, 255, 0, 255, 255, 255, 255, 0, 255, 255};
    const std::string metadata =
        R"({"schema_version":1,"seed":1844674407370955161,"shape":{"family":6}})";
    std::string error;
    expect(glasslight::media::savePngWithSettings(pngPath, source, metadata, error),
           "PNG with settings should save: " + error);

    std::string extracted;
    bool found = false;
    expect(glasslight::media::extractPngSettings(pngPath, extracted, found, error),
           "settings chunk should parse: " + error);
    expect(found, "settings chunk should be present");
    expect(extracted == metadata, "settings JSON should round-trip byte-for-byte");
    glasslight::CompositionSettings migrated;
    std::string migrationMessage;
    expect(glasslight::deserializeSettings(extracted, migrated, migrationMessage),
           "v1 PNG composition metadata should deserialize and migrate");
    expect(migrated.schemaVersion == 2u && migrated.fractureShardCount == 9u &&
               migrated.fractureMorphology == glasslight::FractureMorphology::Auto &&
               migrated.fractureMotion == glasslight::FractureMotion::Auto,
           "v1 PNG restoration should install Fracture v2 defaults");

    RgbaImage loaded;
    extracted.clear();
    expect(glasslight::media::loadPngWithSettings(pngPath, loaded, extracted, error),
           "PNG pixels should load: " + error);
    expect(loaded.width == source.width && loaded.height == source.height,
           "PNG dimensions should round-trip");
    expect(loaded.pixels == source.pixels, "RGBA pixels should round-trip exactly");
    expect(extracted == metadata, "load helper should return embedded settings");

    // Saving to the same destination exercises same-directory atomic replacement
    // and must leave exactly one glAs chunk.
    const std::string replacement = R"({"schemaVersion":1,"seed":42})";
    expect(glasslight::media::savePngWithSettings(pngPath, source, replacement, error),
           "existing PNG should be atomically replaceable: " + error);
    expect(glasslight::media::extractPngSettings(pngPath, extracted, found, error),
           "replacement metadata should parse: " + error);
    expect(extracted == replacement, "replacement settings should supersede old settings");

    // Flip a byte in glAs data without updating its CRC.
    std::vector<std::uint8_t> corrupt = readBytes(pngPath);
    const std::array<std::uint8_t, 4> chunkName{'g', 'l', 'A', 's'};
    const auto chunk = std::search(corrupt.begin(), corrupt.end(),
                                   chunkName.begin(), chunkName.end());
    expect(chunk != corrupt.end(), "saved PNG should contain glAs bytes");
    if (chunk != corrupt.end() && chunk + 4 != corrupt.end()) {
        *(chunk + 4) ^= 0x01u;
        const std::filesystem::path corruptPath = directory / "corrupt.png";
        writeBytes(corruptPath, corrupt);
        expect(!glasslight::media::extractPngSettings(
                   corruptPath, extracted, found, error),
               "corrupted settings CRC should be rejected");
        expect(error.find("CRC") != std::string::npos,
               "corrupted settings should report a CRC error");
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0) {
        std::cout << "PNG metadata tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
