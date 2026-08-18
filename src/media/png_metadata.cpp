#include "media/png_metadata.hpp"
#include "platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

namespace glasslight::media {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
constexpr std::array<std::uint8_t, 4> kSettingsChunk{'g', 'l', 'A', 's'};
constexpr std::size_t kMaximumSettingsBytes = 16u * 1024u * 1024u;

std::uint32_t readBigEndian32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

void appendBigEndian32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
    output.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    output.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    output.push_back(static_cast<std::uint8_t>(value & 0xffu));
}

std::uint32_t updateCrc(std::uint32_t crc, const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

std::uint32_t chunkCrc(const std::uint8_t* type,
                       const std::uint8_t* data,
                       std::size_t size) {
    std::uint32_t crc = updateCrc(0xffffffffu, type, 4u);
    crc = updateCrc(crc, data, size);
    return crc ^ 0xffffffffu;
}

bool readFile(const std::filesystem::path& path,
              std::vector<std::uint8_t>& bytes,
              std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open PNG: " + platform::pathToUtf8(path);
        return false;
    }
    const std::streamsize size = input.tellg();
    if (size < 0) {
        error = "Could not determine PNG size: " + platform::pathToUtf8(path);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error = "Could not read PNG: " + platform::pathToUtf8(path);
        return false;
    }
    return true;
}

bool writeFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes,
               std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not create PNG: " + platform::pathToUtf8(path);
        return false;
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) {
        error = "Could not finish writing PNG: " + platform::pathToUtf8(path);
        return false;
    }
    return true;
}

std::filesystem::path temporarySibling(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return path.parent_path() / platform::pathFromUtf8(
        platform::pathToUtf8(path.filename()) + ".tmp." + std::to_string(tick) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
}

void appendSettingsChunk(std::vector<std::uint8_t>& output,
                         const std::string& settingsJson) {
    appendBigEndian32(output, static_cast<std::uint32_t>(settingsJson.size()));
    output.insert(output.end(), kSettingsChunk.begin(), kSettingsChunk.end());
    const auto* data = reinterpret_cast<const std::uint8_t*>(settingsJson.data());
    output.insert(output.end(), data, data + settingsJson.size());
    appendBigEndian32(output, chunkCrc(kSettingsChunk.data(), data, settingsJson.size()));
}

bool insertSettingsChunk(const std::vector<std::uint8_t>& png,
                         const std::string& settingsJson,
                         std::vector<std::uint8_t>& output,
                         std::string& error) {
    if (settingsJson.size() > kMaximumSettingsBytes ||
        settingsJson.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "Composition metadata exceeds the 16 MiB safety limit.";
        return false;
    }
    if (png.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), png.begin())) {
        error = "SDL produced an invalid PNG signature.";
        return false;
    }

    output.clear();
    output.reserve(png.size() + settingsJson.size() + 12u);
    output.insert(output.end(), kPngSignature.begin(), kPngSignature.end());
    bool sawIend = false;
    std::size_t cursor = kPngSignature.size();
    while (cursor < png.size()) {
        if (png.size() - cursor < 12u) {
            error = "PNG contains a truncated chunk.";
            return false;
        }
        const std::uint32_t length = readBigEndian32(png.data() + cursor);
        const std::size_t chunkSize = static_cast<std::size_t>(length) + 12u;
        if (chunkSize > png.size() - cursor) {
            error = "PNG chunk length exceeds the file size.";
            return false;
        }
        const std::uint8_t* type = png.data() + cursor + 4u;
        const bool isSettings = std::equal(kSettingsChunk.begin(), kSettingsChunk.end(), type);
        const bool isIend = std::memcmp(type, "IEND", 4u) == 0;
        if (isIend) {
            appendSettingsChunk(output, settingsJson);
            sawIend = true;
        }
        if (!isSettings) {
            output.insert(output.end(), png.begin() + static_cast<std::ptrdiff_t>(cursor),
                          png.begin() + static_cast<std::ptrdiff_t>(cursor + chunkSize));
        }
        cursor += chunkSize;
        if (isIend) {
            if (cursor != png.size()) {
                error = "PNG contains trailing data after IEND.";
                return false;
            }
            break;
        }
    }
    if (!sawIend) {
        error = "PNG is missing its IEND chunk.";
        return false;
    }
    return true;
}

bool parseSettingsChunk(const std::vector<std::uint8_t>& png,
                        std::string& settingsJson,
                        bool& found,
                        std::string& error) {
    found = false;
    settingsJson.clear();
    if (png.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), png.begin())) {
        error = "File is not a PNG.";
        return false;
    }

    bool sawIend = false;
    std::size_t cursor = kPngSignature.size();
    while (cursor < png.size()) {
        if (png.size() - cursor < 12u) {
            error = "PNG contains a truncated chunk.";
            return false;
        }
        const std::uint32_t length = readBigEndian32(png.data() + cursor);
        const std::size_t chunkSize = static_cast<std::size_t>(length) + 12u;
        if (chunkSize > png.size() - cursor) {
            error = "PNG chunk length exceeds the file size.";
            return false;
        }
        const std::uint8_t* type = png.data() + cursor + 4u;
        const std::uint8_t* data = type + 4u;
        const std::uint32_t storedCrc = readBigEndian32(data + length);
        if (std::equal(kSettingsChunk.begin(), kSettingsChunk.end(), type)) {
            if (length > kMaximumSettingsBytes) {
                error = "Composition metadata exceeds the 16 MiB safety limit.";
                return false;
            }
            if (chunkCrc(type, data, length) != storedCrc) {
                error = "Composition metadata has an invalid PNG CRC.";
                return false;
            }
            if (found) {
                error = "PNG contains more than one composition metadata chunk.";
                return false;
            }
            settingsJson.assign(reinterpret_cast<const char*>(data), length);
            found = true;
        }
        if (std::memcmp(type, "IEND", 4u) == 0) {
            sawIend = true;
            cursor += chunkSize;
            if (cursor != png.size()) {
                error = "PNG contains trailing data after IEND.";
                return false;
            }
            break;
        }
        cursor += chunkSize;
    }
    if (!sawIend) {
        error = "PNG is missing its IEND chunk.";
        return false;
    }
    return true;
}

} // namespace

bool savePngWithSettings(const std::filesystem::path& path,
                         const RgbaImage& image,
                         const std::string& settingsJson,
                         std::string& error) {
    error.clear();
    if (image.width == 0u || image.height == 0u ||
        image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        error = "PNG dimensions are invalid.";
        return false;
    }
    const std::uint64_t expectedSize =
        static_cast<std::uint64_t>(image.width) * image.height * 4u;
    if (expectedSize != image.pixels.size()) {
        error = "RGBA pixel data does not match the PNG dimensions.";
        return false;
    }
    if (!path.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError) {
            error = "Could not create PNG directory: " + directoryError.message();
            return false;
        }
    }

    const std::filesystem::path temporary = temporarySibling(path);
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        static_cast<int>(image.width), static_cast<int>(image.height),
        SDL_PIXELFORMAT_RGBA32, const_cast<std::uint8_t*>(image.pixels.data()),
        static_cast<int>(image.width * 4u));
    if (!surface) {
        error = "Could not create SDL PNG surface: " + std::string(SDL_GetError());
        return false;
    }
    const std::string temporaryUtf8 = platform::pathToUtf8(temporary);
    const bool encoded = SDL_SavePNG(surface, temporaryUtf8.c_str());
    SDL_DestroySurface(surface);
    if (!encoded) {
        error = "Could not encode PNG: " + std::string(SDL_GetError());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    std::vector<std::uint8_t> png;
    std::vector<std::uint8_t> withMetadata;
    if (!readFile(temporary, png, error) ||
        !insertSettingsChunk(png, settingsJson, withMetadata, error) ||
        !writeFile(temporary, withMetadata, error)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    std::string replaceError;
    if (!platform::replaceFile(temporary, path, replaceError)) {
        error = "Could not replace PNG atomically: " + replaceError;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool extractPngSettings(const std::filesystem::path& path,
                        std::string& settingsJson,
                        bool& found,
                        std::string& error) {
    error.clear();
    std::vector<std::uint8_t> png;
    return readFile(path, png, error) &&
           parseSettingsChunk(png, settingsJson, found, error);
}

bool loadPngWithSettings(const std::filesystem::path& path,
                         RgbaImage& image,
                         std::string& settingsJson,
                         std::string& error) {
    bool found = false;
    if (!extractPngSettings(path, settingsJson, found, error)) {
        return false;
    }
    if (!found) {
        settingsJson.clear();
    }

    const std::string pathUtf8 = platform::pathToUtf8(path);
    SDL_Surface* loaded = SDL_LoadPNG(pathUtf8.c_str());
    if (!loaded) {
        error = "Could not decode PNG: " + std::string(SDL_GetError());
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba) {
        error = "Could not convert PNG to RGBA: " + std::string(SDL_GetError());
        return false;
    }
    if (rgba->w <= 0 || rgba->h <= 0 || !SDL_LockSurface(rgba)) {
        error = "Could not access decoded PNG pixels: " + std::string(SDL_GetError());
        SDL_DestroySurface(rgba);
        return false;
    }

    image.width = static_cast<std::uint32_t>(rgba->w);
    image.height = static_cast<std::uint32_t>(rgba->h);
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4u);
    for (std::uint32_t row = 0; row < image.height; ++row) {
        const auto* source = static_cast<const std::uint8_t*>(rgba->pixels) +
                             static_cast<std::size_t>(row) * rgba->pitch;
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(row) * image.width * 4u,
                    source, static_cast<std::size_t>(image.width) * 4u);
    }
    SDL_UnlockSurface(rgba);
    SDL_DestroySurface(rgba);
    return true;
}

} // namespace glasslight::media
