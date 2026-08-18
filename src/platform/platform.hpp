#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace glasslight::platform {

std::filesystem::path pathFromUtf8(std::string_view value);
std::string pathToUtf8(const std::filesystem::path& value);

std::filesystem::path configDirectory();

bool durableWriteExclusive(const std::filesystem::path& path,
                           std::string_view payload,
                           std::string& error);
bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string& error);

} // namespace glasslight::platform
