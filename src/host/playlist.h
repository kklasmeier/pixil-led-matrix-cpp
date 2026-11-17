// Playlist resolution for the host: mm:ss durations, debug levels, and
// script paths relative to the scripts directory, including one-segment
// wildcards such as main/*.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pixil {

enum class DebugLevel { Off = 0, Concise = 1, Summary = 2, Verbose = 3 };

struct Playlist {
    std::vector<std::filesystem::path> scripts;
    bool wildcard = false;
};

// "mm:ss" with seconds in 00..59. Minutes may exceed 59, so "60:00" is one hour.
int parse_duration(const std::string& text);

DebugLevel parse_debug_level(const std::string& text);

// Resolve each pattern. An existing path is used as given. Otherwise the path
// is tried under scripts_dir, with ".pix" added when the file has no suffix.
// A pattern containing '*' matches .pix files in that one directory. The
// result is sorted and de-duplicated.
Playlist collect_scripts(const std::vector<std::string>& patterns,
                         const std::filesystem::path& scripts_dir);

}  // namespace pixil
