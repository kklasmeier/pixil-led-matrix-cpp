#include "check.h"

#include "host/playlist.h"
#include "runtime/value.h"

using namespace pixil;

TEST(playlist_duration_and_debug_level) {
    CHECK(parse_duration("0:05") == 5);
    CHECK(parse_duration("2:30") == 150);
    CHECK(parse_duration("60:00") == 3600);
    bool rejected = false;
    try {
        parse_duration("1:60");
    } catch (const PixilError&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(parse_debug_level("debug_summary") == DebugLevel::Summary);
    CHECK(parse_debug_level("DEBUG_VERBOSE") == DebugLevel::Verbose);
}

TEST(playlist_resolves_scripts_under_scripts_dir) {
    const Playlist one = collect_scripts({"main/Cloth_simulation"}, "scripts");
    CHECK(one.scripts.size() == 1);
    CHECK(!one.wildcard);
    CHECK(one.scripts[0].filename() == "Cloth_simulation.pix");

    const Playlist glob = collect_scripts({"main/Cloth_simulation*"}, "scripts");
    CHECK(glob.wildcard);
    CHECK(glob.scripts.size() == 1);
    CHECK(glob.scripts[0].filename() == "Cloth_simulation.pix");
}
