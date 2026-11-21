// tools/lighting_config: lighting.json of the game folder: read with defaults, write merged, refuse what the Core would not read (T11.10d).
#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

#include "tools/lighting_config.h"

using namespace native::tools;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Dir {
    fs::path root;
    Dir() {
        root = fs::temp_directory_path() / ("lighting_cfg_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path file() const { return root / "lighting.json"; }
    void put(const std::string& text) const { std::ofstream(file(), std::ios::binary) << text; }
    json get() const {
        std::ifstream in(file(), std::ios::binary);
        return json::parse(in);
    }
};

} // namespace

TEST_CASE("lighting config: every key of the groups is a key the Core has, once") {
    Dir d;
    const LightingConfig c = readLightingConfig(d.file());
    std::set<std::string> seen;
    for (const LightingGroup& g : lightingGroups())
        for (const LightingKey& k : g.keys) {
            INFO("key " << k.key);
            CHECK(c.values.contains(k.key));
            CHECK(seen.insert(k.key).second);
            CHECK_FALSE(k.label.empty());
        }
    CHECK(seen.size() >= 35);
}

TEST_CASE("lighting config: a missing file reads as the defaults") {
    Dir d;
    const LightingConfig c = readLightingConfig(d.file());
    CHECK(c.warning.empty());
    CHECK(c.values == c.defaults);
    CHECK(c.values["texelsPerMeter"].is_number());
    CHECK(c.values["useGpu"].is_boolean());
}

TEST_CASE("lighting config: a value in the file wins, a wild one is clamped, a broken file warns") {
    Dir d;
    d.put(R"({"texelsPerMeter": 16, "maxPasses": 5})");
    LightingConfig c = readLightingConfig(d.file());
    CHECK(c.values["texelsPerMeter"] == 16);
    CHECK(c.values["maxPasses"] == 5);
    CHECK(c.values["atlasSize"] == c.defaults["atlasSize"]);
    d.put("this is not json");
    c = readLightingConfig(d.file());
    CHECK_FALSE(c.warning.empty());
    CHECK(c.values == c.defaults);
}

TEST_CASE("lighting config: writing merges, keeps unknown keys, creates the file") {
    Dir d;
    CHECK(writeLightingConfig(d.file(), json{{"texelsPerMeter", 32}}).empty());
    CHECK(d.get()["texelsPerMeter"] == 32);
    d.put(R"({"texelsPerMeter": 16, "myOwnNote": "keep me", "useGpu": true})");
    CHECK(writeLightingConfig(d.file(), json{{"useGpu", false}, {"maxPasses", 9}}).empty());
    const json j = d.get();
    CHECK(j["texelsPerMeter"] == 16);
    CHECK(j["myOwnNote"] == "keep me");
    CHECK(j["useGpu"] == false);
    CHECK(j["maxPasses"] == 9);
    CHECK(readLightingConfig(d.file()).values["maxPasses"] == 9);
    CHECK_FALSE(fs::exists(fs::path(d.file()).concat(".tmp")));
    // a folder that is not there yet
    const fs::path nested = d.root / "sub" / "lighting.json";
    CHECK(writeLightingConfig(nested, json{{"padding", 2}}).empty());
    CHECK(fs::exists(nested));
}

TEST_CASE("lighting config: unknown keys, wrong kinds of values and non-objects are refused and change nothing") {
    Dir d;
    d.put(R"({"texelsPerMeter": 16})");
    CHECK(writeLightingConfig(d.file(), json{{"nonsense", 1}}).find("unknown") == 0);
    CHECK(writeLightingConfig(d.file(), json{{"useGpu", 1}}).find("wrong kind") == 0);
    CHECK(writeLightingConfig(d.file(), json{{"texelsPerMeter", "lots"}}).find("wrong kind") == 0);
    CHECK_FALSE(writeLightingConfig(d.file(), json::array()).empty());
    CHECK(d.get() == json::parse(R"({"texelsPerMeter": 16})"));
}
