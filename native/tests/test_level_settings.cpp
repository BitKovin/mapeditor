// doc/level_settings: worldspawn keys with defaults, typed values to stored keys, ambient, sky, and the multi-key edit (T11.10a).
#include <doctest.h>

#include <cmath>

#include "doc/io.h"
#include "doc/level_settings.h"

using namespace native::doc;

namespace {

std::shared_ptr<const DocumentVersion> emptyDoc() {
    const LoadResult r = loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    return r.version;
}

std::shared_ptr<const DocumentVersion> docWithWorld(const std::string& keys) {
    const LoadResult r = loadDocumentText(R"({"schemaVersion": "0.2", "entities": [{"id": "e_w", "classname": "worldspawn", )" + keys + R"("brushes": []}]})");
    REQUIRE(r.version);
    return r.version;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

TEST_CASE("level settings: the tables are what the Shell shows") {
    const auto& sections = levelSections();
    REQUIRE(sections.size() == 7);
    size_t fields = 0;
    for (const LevelSection& s : sections) fields += s.fields.size();
    CHECK(fields == 39);
    const LevelField* scale = levelField("_light_scale");
    REQUIRE(scale);
    CHECK(scale->dflt.value() == 1);
    CHECK(scale->max == 100);
    const LevelField* exposure = levelField("_cc_exposure");
    REQUIRE(exposure);
    CHECK_FALSE(exposure->dflt.has_value());   // empty = the Core's lighting.json value
    CHECK(levelField("_ao_rays")->isInt);
    CHECK(levelField("_nope") == nullptr);
    CHECK(levelField("_relief_distance")->hint.find("Export only") == 0);
}

TEST_CASE("level settings: a stored value, an absent key and junk") {
    const auto v = docWithWorld(R"("keyvalues": {"_ao_radius": "2.5", "_ao_rays": " ", "_light_scale": "abc"}, )");
    const core::Entity* w = worldspawnOf(*v);
    REQUIRE(w);
    CHECK(near(*levelValue(w, *levelField("_ao_radius")), 2.5));
    CHECK_FALSE(levelValue(w, *levelField("_ao_rays")).has_value());      // blank
    CHECK_FALSE(levelValue(w, *levelField("_light_scale")).has_value());  // not a number
    CHECK_FALSE(levelValue(w, *levelField("_ao_power")).has_value());     // absent
    CHECK_FALSE(levelValue(nullptr, *levelField("_ao_power")).has_value());
    CHECK(worldspawnOf(*emptyDoc()) == nullptr);
}

TEST_CASE("level settings: what a typed value stores") {
    const LevelField& scale = *levelField("_light_scale");   // default 1, 0..100
    CHECK_FALSE(levelKeyFor(scale, "").has_value());          // empty = the default
    CHECK_FALSE(levelKeyFor(scale, "1").has_value());         // the default itself
    CHECK_FALSE(levelKeyFor(scale, "abc").has_value());
    CHECK(*levelKeyFor(scale, "2") == "2");
    CHECK(*levelKeyFor(scale, "0.25") == "0.25");
    CHECK(*levelKeyFor(scale, "500") == "100");                // clamped
    CHECK(*levelKeyFor(scale, "-3") == "0");
    const LevelField& rays = *levelField("_ao_rays");          // integer, default 32
    CHECK(*levelKeyFor(rays, "64.6") == "65");
    CHECK_FALSE(levelKeyFor(rays, "32.2").has_value());        // rounds to the default
    const LevelField& exposure = *levelField("_cc_exposure");  // no default: any value is stored
    CHECK(*levelKeyFor(exposure, "1") == "1");
    CHECK(*levelKeyFor(exposure, "0.001") == "0.05");
    CHECK(levelNumber(0.1 + 0.2) == "0.3");
    CHECK(levelNumber(-0.0) == "0");
}

TEST_CASE("level settings: ambient colour and intensity") {
    const auto v = docWithWorld(R"("keyvalues": {"_ambient": "0.2 0.1 0.05"}, )");
    const Ambient a = ambientOf(worldspawnOf(*v));
    CHECK(near(a.intensity, 0.2));
    CHECK(near(a.color[0], 1));
    CHECK(near(a.color[1], 0.5));
    CHECK(near(a.color[2], 0.25));
    CHECK(*ambientKey(a.color, a.intensity) == "0.2 0.1 0.05");
    CHECK_FALSE(ambientKey({1, 1, 1}, 0).has_value());
    const Ambient none = ambientOf(nullptr);
    CHECK(none.intensity == 0);
    for (const char* bad : {"1 2", "1 2 3 4", "a b c", "-1 0 0", "0 0 0"}) {
        const auto d = docWithWorld(std::string(R"("keyvalues": {"_ambient": ")") + bad + R"("}, )");
        CHECK(ambientOf(worldspawnOf(*d)).intensity == 0);
    }
    CHECK(hexOfColor({1, 0.5, 0}) == "#ff8000");
    const auto c = colorOfHex("#ff8000");
    CHECK(near(c[0], 1));
    CHECK(near(c[2], 0));
    CHECK(colorOfHex("nonsense")[1] == 1);
}

TEST_CASE("level settings: sky keys") {
    const auto v = docWithWorld(R"("keyvalues": {"_sky_auto": "areas", "_sky_margin": "20"}, )");
    const core::Entity* w = worldspawnOf(*v);
    CHECK(autoSkyOf(w) == AutoSky::Areas);
    CHECK(near(skyMarginOf(w), 20));
    CHECK(autoSkyOf(nullptr) == AutoSky::Off);
    CHECK(near(skyMarginOf(nullptr), kSkyMarginDefault));
    CHECK_FALSE(skyMarginKey("8").has_value());   // the default
    CHECK_FALSE(skyMarginKey("").has_value());
    CHECK(*skyMarginKey("12.5") == "12.5");
    CHECK(*skyMarginKey("99999") == "1000");
    CHECK(autoSkyOptions().size() == 3);
    CHECK(isCubemapPath("env/sunsetcube"));
    CHECK_FALSE(isCubemapPath("env/Sunset_Cube"));   // case-sensitive, as the engine checks it
    CHECK(isEnvSkybox("env/sunsetcube"));
    CHECK_FALSE(isEnvSkybox("textures/skycube"));
}

TEST_CASE("level settings: setLevelKeys writes several keys as one version") {
    const auto base = docWithWorld(R"("keyvalues": {"_ao_radius": "2", "_ambient": "1 1 1"}, )");
    const auto next = setLevelKeys(base, {{"_ao_radius", std::nullopt}, {"_sky_auto", "box"}, {"_light_scale", "3"}});
    REQUIRE(next);
    const core::Entity* w = worldspawnOf(*next);
    CHECK(w->keyvalues.count("_ao_radius") == 0);
    CHECK(w->keyvalues.at("_sky_auto") == "box");
    CHECK(w->keyvalues.at("_light_scale") == "3");
    CHECK(w->keyvalues.at("_ambient") == "1 1 1");   // untouched keys stay
    CHECK(next->version != base->version);
    // nothing to change gives no version
    CHECK(setLevelKeys(base, {{"_ao_radius", "2"}}) == nullptr);
    CHECK(setLevelKeys(base, {{"_nothing", std::nullopt}}) == nullptr);
    CHECK(setLevelKeys(nullptr, {{"a", "b"}}) == nullptr);
}

TEST_CASE("level settings: the worldspawn is made when a value is set and there is none") {
    const auto base = emptyDoc();
    CHECK(setLevelKeys(base, {{"_ao_radius", std::nullopt}}) == nullptr);   // removing from nothing is nothing
    const auto next = setLevelKeys(base, {{"_ao_radius", "4"}});
    REQUIRE(next);
    const core::Entity* w = worldspawnOf(*next);
    REQUIRE(w);
    CHECK(w->id == "e_worldspawn");
    CHECK(w->keyvalues.at("_ao_radius") == "4");
}
