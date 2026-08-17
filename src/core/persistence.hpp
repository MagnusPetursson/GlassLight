#pragma once

#include "core/composition.hpp"

#include <filesystem>
#include <string>

namespace glasslight {

// Resolves to $XDG_CONFIG_HOME/glasslight/settings.json, with the conventional
// per-user config directory as a fallback. The path is not created here.
std::filesystem::path lastSessionPath();

// A missing settings file is a successful load with found == false. A malformed
// or unreadable existing file returns false and leaves settings unchanged.
bool loadLastSession(CompositionSettings& settings, bool& found, std::string& error);

// Writes a complete canonical payload to a sibling temporary file and replaces
// the destination atomically. Parent directories are created as needed.
bool saveLastSession(const CompositionSettings& settings, std::string& error);

} // namespace glasslight
