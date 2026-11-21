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
