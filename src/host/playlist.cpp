#include "host/playlist.h"

#include <algorithm>
#include <cctype>

#include "runtime/value.h"

namespace pixil {
namespace {

bool all_digits(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// '*' matches any run of characters. The pattern does not cross a directory.
bool match_star(const std::string& pattern, const std::string& name) {
    std::size_t pi = 0;
    std::size_t ni = 0;
    std::size_t star = std::string::npos;
    std::size_t mark = 0;
    while (ni < name.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            mark = ni;
        } else if (pi < pattern.size() && pattern[pi] == name[ni]) {
            ++pi;
            ++ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

void append_matches(std::vector<std::filesystem::path>& out, const std::filesystem::path& dir,
                    const std::string& name_pattern) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const fs::path file = entry.path();
        if (file.extension() != ".pix") continue;
        if (match_star(name_pattern, file.filename().string())) out.push_back(file);
    }
}

std::filesystem::path resolve_one(const std::string& pattern, const std::filesystem::path& scripts_dir) {
    namespace fs = std::filesystem;
    fs::path given(pattern);
    std::error_code ec;
    if (fs::is_regular_file(given, ec)) return given;
    fs::path under = scripts_dir / given;
    if (fs::is_regular_file(under, ec)) return under;
    if (under.extension().empty()) {
        under.replace_extension(".pix");
        if (fs::is_regular_file(under, ec)) return under;
    }
    throw PixilError(ErrorKind::Value, "script not found: " + pattern);
}

}  // namespace

int parse_duration(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos) {
        throw PixilError(ErrorKind::Value, "duration must be mm:ss, got '" + text + "'");
    }
    const std::string minutes = text.substr(0, colon);
    const std::string seconds = text.substr(colon + 1);
    if (!all_digits(minutes) || seconds.size() != 2 || !all_digits(seconds)) {
        throw PixilError(ErrorKind::Value, "duration must be mm:ss, got '" + text + "'");
    }
    int sec = std::stoi(seconds);
    if (sec > 59) throw PixilError(ErrorKind::Value, "duration seconds must be 00..59, got '" + text + "'");
    long long total = std::stoll(minutes) * 60LL + sec;
    if (total > 86400) throw PixilError(ErrorKind::Value, "duration is longer than a day: " + text);
    return static_cast<int>(total);
}

DebugLevel parse_debug_level(const std::string& text) {
    std::string upper = text;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "DEBUG_OFF") return DebugLevel::Off;
    if (upper == "DEBUG_CONCISE") return DebugLevel::Concise;
    if (upper == "DEBUG_SUMMARY") return DebugLevel::Summary;
    if (upper == "DEBUG_VERBOSE") return DebugLevel::Verbose;
    throw PixilError(ErrorKind::Value,
                     "debug level must be DEBUG_OFF, DEBUG_CONCISE, DEBUG_SUMMARY, or DEBUG_VERBOSE");
}

Playlist collect_scripts(const std::vector<std::string>& patterns, const std::filesystem::path& scripts_dir) {
    namespace fs = std::filesystem;
    Playlist playlist;
    std::vector<fs::path> found;
    for (const std::string& pattern : patterns) {
        if (pattern.empty()) throw PixilError(ErrorKind::Value, "empty script pattern");
        if (pattern.find('*') == std::string::npos) {
            found.push_back(resolve_one(pattern, scripts_dir));
            continue;
        }
        playlist.wildcard = true;
        const fs::path as_path(pattern);
        const std::string name = as_path.filename().string();
        const fs::path parent = as_path.parent_path();
        const std::size_t before = found.size();
        append_matches(found, parent.empty() ? fs::path(".") : parent, name);
        fs::path under = scripts_dir;
        std::string relative = pattern;
        const std::string scripts_prefix = "scripts/";
        if (relative.rfind(scripts_prefix, 0) == 0) relative = relative.substr(scripts_prefix.size());
        under /= fs::path(relative).parent_path();
        append_matches(found, under, name);
        if (found.size() == before) {
            throw PixilError(ErrorKind::Value, "no scripts match " + pattern);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    playlist.scripts = std::move(found);
    if (playlist.scripts.empty()) throw PixilError(ErrorKind::Value, "no scripts to run");
    return playlist;
}

}  // namespace pixil
