#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace glasslight::media {

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

// Writes an SDL-encoded RGBA PNG and embeds settingsJson in the private,
// ancillary glAs chunk. The destination is replaced atomically where the
// platform supports replacing a file with rename().
bool savePngWithSettings(const std::filesystem::path& path,
                         const RgbaImage& image,
                         const std::string& settingsJson,
                         std::string& error);

// Decodes pixels through SDL and validates/extracts the optional glAs chunk.
bool loadPngWithSettings(const std::filesystem::path& path,
                         RgbaImage& image,
                         std::string& settingsJson,
                         std::string& error);

// Reads only the private settings chunk. A valid PNG without glAs succeeds
// with found set to false.
bool extractPngSettings(const std::filesystem::path& path,
                        std::string& settingsJson,
                        bool& found,
                        std::string& error);

} // namespace glasslight::media
