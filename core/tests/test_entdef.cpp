#include "core/lights.h"
#include "core/entdef.h"
#include "core/profile.h"
#include "core/router.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

// A file in the style of GtkRadiant/NetRadiant's entities.ent (Quake 3).
static const char* kSample = R"(<?xml version="1.0"?>
<!--
  comment with <point name="ghost"> inside must be ignored
-->
<classes>
<group name="worldspawn" color="0 0 0">
The world entity.
<string key="message" name="Message">Map title</string>
<integer key="_blocksize" name="Block size">Vis block size</integer>
</group>

<point name="light" color="1 1 0" box="-8 -8 -8 8 8 8">
Invisible light source. The default intensity is 300.
-------- KEYS --------
this must not end up in the description
<real key="light" name="Intensity">Light intensity.</real>
<color1 key="_color" name="Colour">RGB 0-1</color1>
<target key="target" name="Target"/>
<flag key="LINEAR" name="Linear" bit="0">Linear falloff</flag>
<flag key="NOANGLE" name="No angle" bit="1">Ignore surface angle</flag>
</point>

<point name="info_player_start" color=".2 .8 .2" box='-16 -16 -24 16 16 32'>
Player spawn.
<angle key="angle" name="Angle">Facing</angle>
</point>

<group name="func_door" color="0 .5 .8">
A door.
<list key="lip" name="Lip"><item name="short" value="4"/><item name="long" value="16"/></list>
<flag key="START_OPEN" value="1">Starts open</flag>
</group>
</classes>
)";

TEST_CASE("entdef: parses classes, kinds, colours and boxes") {
    const EntityDefinitions d = parseEntityDefinitions(kSample);
    CHECK(d.warnings.empty());
    REQUIRE(d.classes.size() == 4);  // the commented-out "ghost" is not one of them
    CHECK(d.classes[0].name == "worldspawn");
    CHECK(d.classes[0].kind == EntClass::Kind::Group);
    CHECK_FALSE(d.classes[0].box);

    const EntClass* light = d.find("light");
    REQUIRE(light);
    CHECK(light->kind == EntClass::Kind::Point);
    CHECK(light->color == std::array<float, 3>{1.0f, 1.0f, 0.0f});
    REQUIRE(light->box);
    CHECK((*light->box)[0] == -8.0f);
    CHECK((*light->box)[5] == 8.0f);

    const EntClass* spawn = d.find("info_player_start");
    REQUIRE(spawn);
    CHECK(spawn->color[0] == doctest::Approx(0.2f));  // ".2" without a leading zero, single-quoted box
    CHECK((*spawn->box)[2] == -24.0f);

    CHECK(d.isBrushClass("func_door"));
    CHECK_FALSE(d.isBrushClass("light"));
    CHECK_FALSE(d.isBrushClass("nope"));
}

TEST_CASE("entdef: descriptions, keys, lists and flags") {
    const EntityDefinitions d = parseEntityDefinitions(kSample);
    const EntClass* light = d.find("light");
    REQUIRE(light);
    CHECK(light->description == "Invisible light source. The default intensity is 300.");  // banner and everything after it is dropped
    REQUIRE(light->keys.size() == 3);
    CHECK(light->keys[0].key == "light");
    CHECK(light->keys[0].type == "real");
    CHECK(light->keys[0].name == "Intensity");
    CHECK(light->keys[0].description == "Light intensity.");
    CHECK(light->keys[1].type == "color1");
    CHECK(light->keys[2].key == "target");  // self-closed key
    REQUIRE(light->flags.size() == 2);
    CHECK(light->flags[1].bit == 1);
    CHECK(light->flags[1].name == "No angle");

    const EntClass* door = d.find("func_door");
    REQUIRE(door);
    REQUIRE(door->keys.size() == 1);
    CHECK(door->keys[0].type == "list");
    REQUIRE(door->keys[0].items.size() == 2);
    CHECK(door->keys[0].items[1] == std::make_pair(std::string("long"), std::string("16")));
    REQUIRE(door->flags.size() == 1);
    CHECK(door->flags[0].bit == 0);  // value="1" -> bit 0
    CHECK(door->flags[0].name == "START_OPEN");
}

TEST_CASE("entdef: broken input yields warnings, never an exception or a lost file") {
    const EntityDefinitions d = parseEntityDefinitions(R"(
<classes>
<point color="1 0 0">no name</point>
<point name="a" color="oops" box="1 2 3">bad colour and box</point>
<point name="a">duplicate</point>
<point name="unclosed" color="0 1 0">
<real key="x">never closed
<group name="after">the class before it had no close tag</group>
<point name="flagless"><flag key="F" bit="99"/></point>
</classes>)");
    CHECK(d.warnings.size() >= 5);
    REQUIRE(d.find("a"));
    CHECK(d.find("a")->description == "bad colour and box");  // first one wins
    REQUIRE(d.find("unclosed"));
    CHECK(d.find("unclosed")->keys.size() == 1);
    CHECK(d.find("after"));
    CHECK(d.find("flagless")->flags.empty());

    CHECK(parseEntityDefinitions("").classes.empty());
    CHECK(parseEntityDefinitions("just text, no markup at all").classes.empty());
    CHECK(parseEntityDefinitions("<point name=\"cut off").classes.size() <= 1);  // must simply terminate
    CHECK(parseEntityDefinitions("<<<<>>>> </ < / <point name=x>").classes.size() == 1);
}

TEST_CASE("entdef: JSON form for the Shell") {
    const json j = parseEntityDefinitions(kSample).toJson();
    CHECK(j["type"] == "entity_defs");
    REQUIRE(j["classes"].size() == 4);
    const json& light = j["classes"][1];
    CHECK(light["name"] == "light");
    CHECK(light["kind"] == "point");
    CHECK(light["color"][1] == 1.0);
    CHECK(light["box"].size() == 6);
    CHECK(j["classes"][0]["box"].is_null());
    CHECK(j["classes"][3]["keys"][0]["items"][0]["value"] == "4");
}

TEST_CASE("entdef: loadEntityDefinitions handles missing and oversized files") {
    TempDir d;
    CHECK(loadEntityDefinitions(d.path / "none.ent").classes.empty());
    CHECK(loadEntityDefinitions(d.path / "none.ent").warnings.empty());  // optional file: silence
    std::ofstream(d.path / "e.ent") << kSample;
    CHECK(loadEntityDefinitions(d.path / "e.ent").classes.size() == 4);
}

TEST_CASE("profile: new path fields are read and must stay inside the assets folder") {
    TempDir d;
    CHECK(loadProfile(d.path).entityDefinitions == "entities.ent");
    d.write("game_profile.json", json{{"entityDefinitions", "defs/q3.ent"}, {"mapRoot", "levels"}, {"prefabRoot", "pf"}});
    const GameProfile p = loadProfile(d.path);
    CHECK(p.entityDefinitions == "defs/q3.ent");
    CHECK(p.mapRoot == "levels");
    CHECK(p.toJson()["prefabRoot"] == "pf");
    for (const char* key : {"entityDefinitions", "mapRoot", "prefabRoot"}) {
        d.write("game_profile.json", json{{key, "../outside"}});
        CHECK_THROWS(loadProfile(d.path));
        d.write("game_profile.json", json{{key, "/abs"}});
        CHECK_THROWS(loadProfile(d.path));
        d.write("game_profile.json", json{{key, 5}});
        CHECK_THROWS(loadProfile(d.path));
    }
}

TEST_CASE("router: entity definitions are sent on connect from the profile's .ent file") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}));
    std::ofstream(d.path / "entities.ent") << kSample;
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    Router router(scene);
    const RouterOutput out = router.onConnect();
    bool found = false;
    for (const auto& text : out.toSender) {
        const json m = json::parse(text);
        if (m["type"] == "entity_defs") { found = true; CHECK(m["classes"].size() == 4); }
    }
    CHECK(found);
}

// ---- typed key hints and the modern light classes ------------------------------------------------------------------------------------------

TEST_CASE("entdef: keys carry default / min / max / step; bad numbers warn and are dropped") {
    const EntityDefinitions d = parseEntityDefinitions(R"(
<point name="p" color="1 1 1" box="-8 -8 -8 8 8 8">
<real key="radius" name="Radius" default="10" min="0.1" max="1000" step="0.5">r</real>
<boolean key="cast" name="Cast" default="1">c</boolean>
<real key="bad" name="Bad" min="abc" max="5">b</real>
<string key="plain" name="Plain">p</string>
</point>)");
    const EntClass* c = d.find("p");
    REQUIRE(c);
    REQUIRE(c->keys.size() == 4);
    CHECK(*c->keys[0].defaultValue == "10");
    CHECK(*c->keys[0].min == 0.1);
    CHECK(*c->keys[0].max == 1000);
    CHECK(*c->keys[0].step == 0.5);
    CHECK(*c->keys[1].defaultValue == "1");
    CHECK(!c->keys[1].min);
    CHECK(!c->keys[2].min);          // "abc" is not a number
    CHECK(*c->keys[2].max == 5);
    CHECK(!c->keys[3].defaultValue);
    CHECK(d.warnings.size() == 1);
    const auto j = d.toJson()["classes"][0]["keys"];
    CHECK(j[0]["default"] == "10");
    CHECK(j[0]["min"] == 0.1);
    CHECK(!j[3].contains("default"));
    CHECK(!j[3].contains("min"));
}

TEST_CASE("entdef: NetRadiant's value attribute is the default; an explicit default wins") {
    const EntityDefinitions d = parseEntityDefinitions(R"(
<point name="p" color="1 1 1">
<real key="wait" name="Wait" value="0.3" />
<real3 key="loc" name="Location" value="0 0 0" />
<real key="both" name="Both" value="1" default="2" />
</point>)");
    const EntClass* c = d.find("p");
    REQUIRE(c);
    REQUIRE(c->keys.size() == 3);
    CHECK(*c->keys[0].defaultValue == "0.3");
    CHECK(c->keys[1].type == "real3");
    CHECK(*c->keys[1].defaultValue == "0 0 0");
    CHECK(*c->keys[2].defaultValue == "2");
}

// The game's .ent is the single source of what the editor shows as a light's defaults; the baker reads the same keys. They must agree.
TEST_CASE("entities.ent: the modern light classes exist, are complete, and their defaults are exactly what the baker uses") {
    const std::filesystem::path ent = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets" / "entities.ent";
    const EntityDefinitions defs = loadEntityDefinitions(ent);
    for (const auto& w : defs.warnings) MESSAGE("entdef warning: " << w);
    struct Spec { const char* cls; LightType type; std::vector<const char*> keys; };
    const std::vector<Spec> specs = {
        {"light_point", LightType::Point, {"color", "brightness", "bounce", "cast_shadows", "style", "radius", "source_size"}},
        {"light_spot", LightType::Spot, {"color", "brightness", "bounce", "cast_shadows", "style", "radius", "source_size", "inner_angle", "outer_angle"}},
        {"light_directional", LightType::Directional, {"color", "brightness", "bounce", "cast_shadows", "style", "source_size"}},
        {"light_rect", LightType::Rect, {"color", "brightness", "bounce", "cast_shadows", "style", "radius", "width", "height"}},
        {"light_sky", LightType::Sky, {"color", "brightness", "bounce", "cast_shadows", "style"}}};
    for (const Spec& sp : specs) {
        INFO(sp.cls);
        const EntClass* c = defs.find(sp.cls);
        REQUIRE(c);
        CHECK(c->kind == EntClass::Kind::Point);
        REQUIRE(c->box);
        CHECK(!c->description.empty());
        CHECK(c->keys.size() == sp.keys.size());
        // an entity with no keyvalues: what the baker makes of it
        Document doc;
        Entity e;
        e.id = "x";
        e.classname = sp.cls;
        doc.entities = {e};
        const LightScene scene = collectLights(doc, {});
        REQUIRE(scene.lights.size() == 1);
        const Light& L = scene.lights[0];
        CHECK(L.type == sp.type);
        for (const char* key : sp.keys) {
            INFO(key);
            const EntKey* k = nullptr;
            for (const auto& x : c->keys) if (x.key == key) k = &x;
            REQUIRE(k);
            REQUIRE(k->defaultValue);
            CHECK(!k->name.empty());
            CHECK(!k->description.empty());
            const std::string key_s = key, def = *k->defaultValue;
            const double n = key_s == "color" ? 0 : std::stod(def);
            if (key_s == "color") { CHECK(def == "1 1 1"); CHECK(L.color.x == 1.0); CHECK(L.color.y == 1.0); CHECK(L.color.z == 1.0); }
            else if (key_s == "brightness") CHECK(L.brightness == n);
            else if (key_s == "bounce") CHECK(L.bounce == n);
            else if (key_s == "cast_shadows") CHECK(L.castShadows == (n != 0));
            else if (key_s == "style") CHECK(L.style == static_cast<int>(n));
            else if (key_s == "radius") CHECK(L.radius == n);
            else if (key_s == "source_size") CHECK(L.sourceSize == n);
            else if (key_s == "inner_angle") CHECK(L.innerAngle == n);
            else if (key_s == "outer_angle") CHECK(L.outerAngle == n);
            else if (key_s == "width") CHECK(L.width == n);
            else if (key_s == "height") CHECK(L.height == n);
            // the editor's range never offers a value the baker would clamp
            if (k->min && key_s != "color") {
                Entity lo = e;
                lo.keyvalues[key_s] = std::to_string(*k->min);
                Document d2;
                d2.entities = {lo};
                const Light& Lo = collectLights(d2, {}).lights.at(0);
                const double got = key_s == "brightness" ? Lo.brightness : key_s == "bounce" ? Lo.bounce : key_s == "style" ? Lo.style : key_s == "radius" ? Lo.radius :
                                   key_s == "source_size" ? Lo.sourceSize : key_s == "inner_angle" ? Lo.innerAngle : key_s == "outer_angle" ? Lo.outerAngle :
                                   key_s == "width" ? Lo.width : key_s == "height" ? Lo.height : *k->min;
                if (key_s == "inner_angle") CHECK(got == doctest::Approx(*k->min).epsilon(1e-9));  // inner may be 0
                else CHECK(got == doctest::Approx(*k->min).epsilon(1e-9));
            }
        }
    }
    // the dimension keys are the only ones a class does not have
    CHECK(defs.find("light_sky")->keys.size() == 5);
}
