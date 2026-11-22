// T11.3f: the texture browser's list model (folders, sky folder, search) -- ports of the behaviour of shell/src/ui/contentBrowser.ts. Pure.
#include <doctest.h>

#include "tools/texturing/browser.h"

using namespace native::tools::tex;

namespace {
const std::vector<std::string> kAll = {"base/wall01", "base/Wall02", "base/floor/tile", "env/desert_cube", "sky/clouds", "plain", "trim/metal_sky_blue"};
}

TEST_CASE("browser: folderOf and the sky test follow the Shell") {
    CHECK(folderOf("a/b/c") == "a/b");
    CHECK(folderOf("plain").empty());
    CHECK(isSkyTexture("env/desert_cube"));
    CHECK(isSkyTexture("Sky/Clouds"));
    CHECK(isSkyTexture("something_cube"));
    CHECK_FALSE(isSkyTexture("base/wall01"));
    CHECK_FALSE(isSkyTexture(""));
}

TEST_CASE("browser: the folder list is All, Skyboxes when there is a sky texture, then the folders sorted") {
    const auto f = folderEntries(kAll);
    REQUIRE(f.size() >= 3);
    CHECK(f[0].empty());
    CHECK(f[1] == kSkyFolder);
    CHECK(f[2] == "base");
    CHECK(f.back() == "trim");
    // no sky: no sky entry
    const auto g = folderEntries({"a/b", "a/c"});
    CHECK(g == std::vector<std::string>{"", "a"});
    CHECK(folderLabel("") == "All textures");
    CHECK(folderLabel(kSkyFolder) == "Skyboxes");
    CHECK(folderLabel("base") == "base");
}

TEST_CASE("browser: a folder shows its textures and the ones below it; the search is case-insensitive") {
    CHECK(filterTextures(kAll, "", "").size() == kAll.size());
    CHECK(filterTextures(kAll, "base", "") == std::vector<std::string>{"base/wall01", "base/Wall02", "base/floor/tile"});
    CHECK(filterTextures(kAll, "base/floor", "").size() == 1);
    CHECK(filterTextures(kAll, "base", "WALL").size() == 2);
    CHECK(filterTextures(kAll, "", "metal") == std::vector<std::string>{"trim/metal_sky_blue"});
    // "ba" is not the folder "base"
    CHECK(filterTextures(kAll, "ba", "").empty());
    const auto sky = filterTextures(kAll, kSkyFolder, "");
    CHECK(sky == std::vector<std::string>{"env/desert_cube", "sky/clouds", "trim/metal_sky_blue"});
    CHECK(filterTextures(kAll, kSkyFolder, "clouds").size() == 1);
}

TEST_CASE("browser: the search takes several words, all of which must be in the path") {
    CHECK(filterTextures(kAll, "", "base wall").size() == 2);
    CHECK(filterTextures(kAll, "", "wall base").size() == 2);
    CHECK(filterTextures(kAll, "", "  base   floor ") == std::vector<std::string>{"base/floor/tile"});
    CHECK(filterTextures(kAll, "", "base metal").empty());
}

TEST_CASE("browser: favourites and recent are folders that show up while they hold a texture of the list") {
    const std::vector<std::string> favs = {"trim/metal_sky_blue", "gone/texture", "base/wall01"};
    const std::vector<std::string> recent = {"plain", "base/wall01"};
    const auto f = folderEntries(kAll, favs, recent);
    CHECK(f[0].empty());
    CHECK(f[1] == kFavouritesFolder);
    CHECK(f[2] == kRecentFolder);
    CHECK(f[3] == kSkyFolder);
    CHECK(folderEntries(kAll, {}, {}) == folderEntries(kAll));
    CHECK(folderEntries(kAll, {"gone/texture"}, {}) == folderEntries(kAll));   // nothing of the list exists
    // in the order of the list (the newest recent first), the ones that exist, filtered by the search
    CHECK(filterTextures(kAll, kFavouritesFolder, "", favs, recent) == std::vector<std::string>{"trim/metal_sky_blue", "base/wall01"});
    CHECK(filterTextures(kAll, kRecentFolder, "", favs, recent) == std::vector<std::string>{"plain", "base/wall01"});
    CHECK(filterTextures(kAll, kRecentFolder, "wall", favs, recent) == std::vector<std::string>{"base/wall01"});
    CHECK(filterTextures(kAll, "base", "", favs, recent).size() == 3);   // an ordinary folder is as before
    CHECK(folderLabel(kFavouritesFolder) == "\xE2\x98\x85 Favourites");
    CHECK(folderLabel(kRecentFolder) == "Recent");
}

TEST_CASE("browser: the recent list keeps the newest first without repeats and is limited; toggling adds at the end and removes") {
    std::vector<std::string> r;
    r = pushRecent(r, "a");
    r = pushRecent(r, "b");
    r = pushRecent(r, "a");
    CHECK(r == std::vector<std::string>{"a", "b"});
    r = pushRecent(r, "");
    CHECK(r.size() == 2);
    for (int i = 0; i < 40; ++i) r = pushRecent(r, "t" + std::to_string(i));
    CHECK(r.size() == 24);
    CHECK(r.front() == "t39");
    std::vector<std::string> f = toggled({}, "a");
    f = toggled(f, "b");
    CHECK(f == std::vector<std::string>{"a", "b"});
    f = toggled(f, "a");
    CHECK(f == std::vector<std::string>{"b"});
}
