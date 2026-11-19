// T4.3: hide rules and hidden keys (tools/visibility); converted from shell/tests/hideRules.test.ts.
#include <doctest.h>

#include <nlohmann/json.hpp>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/visibility.h"

using namespace native;
using tools::HideRule;
using doc::SelRef;

namespace {

core::Entity brushEnt(const std::string& id, const std::string& cls, double x) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes.push_back(core::Brush::makeBox(id + "b", {x, 0, 0}, {x + 1, 1, 1}, "dev/grey"));
    return e;
}

std::vector<HideRule> on(std::initializer_list<const char*> patterns) {
    std::vector<HideRule> r;
    for (const char* p : patterns) r.push_back({p, true, false});
    return r;
}

std::shared_ptr<const doc::DocumentVersion> docOf(const std::vector<core::Entity>& entities) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const core::Entity& e : entities) REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::vector<std::string> ids(const std::vector<tools::VisibleEntity>& v) {
    std::vector<std::string> out;
    for (const auto& e : v) out.push_back(e.entityId);
    return out;
}

core::Entity world() {
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("wb", {0, 0, 0}, {1, 1, 1}, "dev/grey"));
    return w;
}

} // namespace

TEST_CASE("hide rules: * is a wildcard, a plain name is exact, case does not matter") {
    const auto m = tools::ruleMatcher(on({"trigger_*", "editor_playable_area", "a.b+c"}));
    for (const char* c : {"trigger_hurt", "Trigger_Push", "trigger_", "editor_playable_area", "EDITOR_PLAYABLE_AREA", "a.b+c"}) CHECK(m(c));
    for (const char* c : {"trigger", "my_trigger_x", "editor_playable_area2", "editor_other", "axb+c", "func_detail", ""}) CHECK_FALSE(m(c));
}

TEST_CASE("hide rules: disabled and empty rules do nothing; worldspawn is never matched") {
    CHECK_FALSE(tools::ruleMatcher({{"trigger_*", false, false}})("trigger_hurt"));
    CHECK_FALSE(tools::ruleMatcher(on({"", "  "}))("anything"));
    const auto all = tools::ruleMatcher(on({"*"}));
    CHECK_FALSE(all("worldspawn"));
    CHECK(all("func_detail"));
}

TEST_CASE("hide rules: addRule normalizes, enables an existing rule, ignores empty input; matchingRule names the rule") {
    const auto a = tools::addRule({}, "  FOO_*  ");
    REQUIRE(a.size() == 1);
    CHECK(a[0].pattern == "foo_*");
    CHECK(a[0].enabled);
    CHECK(a[0].custom);
    const auto r = tools::addRule(tools::defaultHideRules(), "Trigger_*");
    REQUIRE(r.size() == 2);
    CHECK(r[0].enabled);
    CHECK(tools::addRule(r, "   ") == r);
    const std::vector<HideRule> rules = {{"trigger_*", true, false}, {"func_detail", false, false}};
    REQUIRE(tools::matchingRule("trigger_hurt", rules));
    CHECK(tools::matchingRule("trigger_hurt", rules)->pattern == "trigger_*");
    CHECK(tools::matchingRule("func_detail", rules) == nullptr);
}

TEST_CASE("hide rules: restoreHideRules keeps presets, custom rules, drops junk") {
    const nlohmann::json raw = nlohmann::json::parse(R"([
      {"pattern": "trigger_*", "enabled": true}, {"pattern": " FUNC_*", "enabled": false}, {"pattern": "func_*", "enabled": true},
      {"pattern": 5, "enabled": true}, null, {"pattern": "", "enabled": true}])");
    const auto got = tools::restoreHideRules(raw);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == HideRule{"trigger_*", true, false});
    CHECK(got[1] == HideRule{"editor_playable_area", false, false});
    CHECK(got[2] == HideRule{"func_*", true, true});
    CHECK(tools::restoreHideRules(nlohmann::json("nope")) == tools::defaultHideRules());
    CHECK(tools::restoreHideRules(nlohmann::json()) == tools::defaultHideRules());
    CHECK(tools::restoreHideRules(tools::hideRulesToJson(got)) == got);
}

TEST_CASE("visibility: what the views keep, volume classes, key selection") {
    core::Entity point;
    point.id = "p1";
    point.classname = "info_player_start";
    const auto d = docOf({world(), brushEnt("t1", "trigger_hurt", 2), brushEnt("t2", "trigger_push", 4), brushEnt("a1", "editor_playable_area", 6), brushEnt("d1", "func_detail", 8), point});

    tools::HideState hide;
    CHECK(ids(tools::visibleEntities(*d, hide)) == std::vector<std::string>{"w", "t1", "t2", "a1", "d1", "p1"});
    hide.rules = on({"trigger_*", "editor_playable_area"});
    CHECK(ids(tools::visibleEntities(*d, hide)) == std::vector<std::string>{"w", "d1", "p1"});
    hide.rules = on({"trigger_*"});
    hide.keys = {tools::entityKey("d1")};
    CHECK(ids(tools::visibleEntities(*d, hide)) == std::vector<std::string>{"w", "a1", "p1"});
    hide.keys.clear();
    hide.rules = on({"info_*"});
    CHECK(ids(tools::visibleEntities(*d, hide)) == std::vector<std::string>{"w", "t1", "t2", "a1", "d1"});
    // an entity whose brushes are all hidden goes too
    hide.rules = tools::defaultHideRules();
    hide.keys = {tools::brushKey("w", "wb")};
    CHECK(ids(tools::visibleEntities(*d, hide)) == std::vector<std::string>{"t1", "t2", "a1", "d1", "p1"});

    const auto classes = tools::volumeClasses(*docOf({world(), brushEnt("d1", "func_detail", 8), brushEnt("t1", "trigger_hurt", 2), brushEnt("t2", "trigger_push", 4),
                                                       brushEnt("a1", "editor_playable_area", 6), point, brushEnt("t3", "trigger_hurt", 9)}));
    REQUIRE(classes.size() == 4);
    CHECK(classes[0] == tools::VolumeClass{"trigger_hurt", 2});
    CHECK(classes[1] == tools::VolumeClass{"trigger_push", 1});
    CHECK(classes[2] == tools::VolumeClass{"editor_playable_area", 1});
    CHECK(classes[3] == tools::VolumeClass{"func_detail", 1});

    const auto keys = tools::hideKeys({SelRef::brush("w", "wb"), SelRef::entity("t1"), SelRef::instance("p9")});
    CHECK(keys == std::vector<std::string>{"b:w|wb", "e:t1", "i:p9"});
}

TEST_CASE("visibility: hidden things leave the selection") {
    const auto d = docOf({world(), brushEnt("t1", "trigger_hurt", 2), brushEnt("d1", "func_detail", 8)});
    tools::HideState hide;
    hide.rules = on({"trigger_*"});
    const std::vector<SelRef> sel = {SelRef::brush("t1", "t1b"), SelRef::brush("d1", "d1b"), SelRef::brush("w", "wb")};
    CHECK(tools::pruneHidden(*d, sel, hide) == std::vector<SelRef>{SelRef::brush("d1", "d1b"), SelRef::brush("w", "wb")});
    hide.keys = {tools::brushKey("d1", "d1b")};
    CHECK(tools::pruneHidden(*d, sel, hide) == std::vector<SelRef>{SelRef::brush("w", "wb")});
}
