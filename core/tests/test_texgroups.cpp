#include "core/router.h"
#include "core/texgroups.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

static json sample() { return {{"format", 1}, {"slots", {{"a", "dev/a"}}}, {"root", {{"tex", "a"}}}}; }

TEST_CASE("texture groups: write, list (any depth, sorted), read, delete") {
    TempDir d;
    GameProfile p;
    CHECK(listTexGroups(d.path, p).empty());
    CHECK(writeTexGroup(d.path, p, "b", sample()).empty());
    CHECK(writeTexGroup(d.path, p, "buildings/apartment", sample()).empty());
    CHECK(listTexGroups(d.path, p) == std::vector<std::string>{"b", "buildings/apartment"});
    auto g = readTexGroup(d.path, p, "buildings/apartment");
    REQUIRE(g.has_value());
    CHECK((*g)["root"]["tex"] == "a");
    // replacing keeps one file and leaves no temp file behind
    json s2 = sample(); s2["name"] = "second";
    CHECK(writeTexGroup(d.path, p, "b", s2).empty());
    CHECK((*readTexGroup(d.path, p, "b"))["name"] == "second");
    CHECK(!std::filesystem::exists(d.path / "texgroups" / "b.texgroup.json.tmp"));
    CHECK(deleteTexGroup(d.path, p, "b").empty());
    CHECK(deleteTexGroup(d.path, p, "b").empty());  // already gone is fine
    CHECK(!readTexGroup(d.path, p, "b").has_value());
}

TEST_CASE("texture groups: unsafe names, non objects and oversize files are refused") {
    TempDir d;
    GameProfile p;
    for (const char* bad : {"", "../x", "a/../../x", "/abs", "a\\b", "a//b", "a/", "x:y", "a*b", "./x"}) {
        CHECK(!writeTexGroup(d.path, p, bad, sample()).empty());
        CHECK(!readTexGroup(d.path, p, bad).has_value());
        CHECK(!deleteTexGroup(d.path, p, bad).empty());
    }
    CHECK(!writeTexGroup(d.path, p, "x", json::array()).empty());
    CHECK(!writeTexGroup(d.path, p, "x", json{{"format", 1}}).empty());  // neither root nor kit
    // a simple texture set has a kit and no root: it must be accepted (it was refused once: "a group needs a root node")
    CHECK(writeTexGroup(d.path, p, "sets/wall", json{{"format", 1}, {"kit", {{"kind", "wall"}, {"middle", {{"pieces", json::array({"a/b"})}}}}}}).empty());
    CHECK(readTexGroup(d.path, p, "sets/wall").value()["kit"]["kind"] == "wall");
    json big = sample();
    big["description"] = std::string(kTexGroupMaxBytes, 'x');
    CHECK(!writeTexGroup(d.path, p, "x", big).empty());
    // a damaged file on disk reads as missing, never throws
    std::filesystem::create_directories(d.path / "texgroups");
    std::ofstream(d.path / "texgroups" / "broken.texgroup.json") << "{ not json";
    CHECK(!readTexGroup(d.path, p, "broken").has_value());
    // other json files in the folder are not groups
    std::ofstream(d.path / "texgroups" / "notes.json") << "{}";
    CHECK(listTexGroups(d.path, p) == std::vector<std::string>{"broken", "sets/wall"});
}

TEST_CASE("texture groups: the profile moves the folder and keeps it inside the assets root") {
    TempDir d;
    d.write("game_profile.json", json{{"texGroupRoot", "kits"}});
    GameProfile p = loadProfile(d.path);
    CHECK(p.texGroupRoot == "kits");
    CHECK(writeTexGroup(d.path, p, "k", sample()).empty());
    CHECK(std::filesystem::exists(d.path / "kits" / "k.texgroup.json"));
    d.write("game_profile.json", json{{"texGroupRoot", "../outside"}});
    CHECK_THROWS_AS(loadProfile(d.path), std::runtime_error);
    CHECK(GameProfile{}.toJson()["texGroupRoot"] == "texgroups");
}

TEST_CASE("texture groups over the protocol") {
    TempDir d;
    Scene scene{d.path};
    Router router{scene};
    auto send = [&](const json& m) { return json::parse(router.onMessage(m.dump()).toSender.at(0)); };
    json w = send({{"type", "write_texgroup"}, {"requestId", 1}, {"path", "x/y"}, {"group", sample()}});
    CHECK(w["ok"] == true);
    CHECK(send({{"type", "list_texgroups"}, {"requestId", 2}})["texgroups"] == json::array({"x/y"}));
    json r = send({{"type", "read_texgroup"}, {"requestId", 3}, {"path", "x/y"}});
    CHECK(r["ok"] == true);
    CHECK(r["group"]["root"]["tex"] == "a");
    CHECK(send({{"type", "write_texgroup"}, {"requestId", 4}, {"path", "../evil"}, {"group", sample()}})["ok"] == false);
    CHECK(send({{"type", "read_texgroup"}, {"requestId", 5}, {"path", "nope"}})["ok"] == false);
    CHECK(send({{"type", "delete_texgroup"}, {"requestId", 6}, {"path", "x/y"}})["ok"] == true);
    CHECK(send({{"type", "list_texgroups"}, {"requestId", 7}})["texgroups"].empty());
}
