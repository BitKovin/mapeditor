// T4.4: the outliner rows (tools/outliner); converted from shell/tests/outlinerSearch, outlinerFold, outlinerEye and outlinerReveal.test.ts.
#include <doctest.h>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/outliner.h"

using namespace native;
using doc::SelRef;
using tools::EyeState;
using tools::OutlinerState;

namespace {

core::Entity point(const std::string& id, const std::string& cls, const std::string& targetname = "") {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    if (!targetname.empty()) e.keyvalues["targetname"] = targetname;
    return e;
}

core::Entity brushEnt(const std::string& id, const std::string& cls, const std::vector<std::string>& brushIds) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    double x = 0;
    for (const std::string& b : brushIds) {
        e.brushes.push_back(core::Brush::makeBox(b, {x, 0, 0}, {x + 1, 1, 1}, "dev/grey"));
        x += 2;
    }
    return e;
}

std::shared_ptr<const doc::DocumentVersion> docOf(const std::vector<core::Entity>& entities, const std::vector<core::PrefabInstance>& instances = {}) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const core::Entity& e : entities) REQUIRE(ed.addEntity(e));
    for (const core::PrefabInstance& p : instances) REQUIRE(ed.addInstance(p));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::vector<std::string> keys(const tools::OutlinerModel& m) {
    std::vector<std::string> out;
    for (const auto& r : m.rows) out.push_back(r.key);
    return out;
}

const tools::OutlinerRow* row(const tools::OutlinerModel& m, const std::string& key) {
    for (const auto& r : m.rows)
        if (r.key == key) return &r;
    return nullptr;
}

OutlinerState all(std::initializer_list<const char*> expanded, std::string query = "") {
    OutlinerState s;
    s.query = std::move(query);
    for (const char* e : expanded) s.expanded.insert(e);
    return s;
}

} // namespace

TEST_CASE("outliner search: shows everything while the box is empty") {
    const auto d = docOf({point("p1", "info_player_start"), point("l1", "light", "hall_lamp"), brushEnt("d1", "func_detail", {"d1a", "d1b"})});
    CHECK(tools::buildOutliner(*d, {}, {}, all({"d1"})).rows.size() == 5);
}

TEST_CASE("outliner search: filters by class, case-insensitively, by id and by target name") {
    const auto d = docOf({point("p1", "info_player_start"), point("l1", "light", "hall_lamp"), brushEnt("d1", "func_detail", {"d1a", "d1b"})});
    CHECK(keys(tools::buildOutliner(*d, {}, {}, all({"d1"}, "LIGHT"))) == std::vector<std::string>{"e:l1"});
    CHECK(keys(tools::buildOutliner(*d, {}, {}, all({"d1"}, "hall_la"))) == std::vector<std::string>{"e:l1"});
    CHECK(keys(tools::buildOutliner(*d, {}, {}, all({"d1"}, "p1"))) == std::vector<std::string>{"e:p1"});
    const auto m = tools::buildOutliner(*d, {}, {}, all({"d1"}));
    CHECK(row(m, "e:l1")->target == "hall_lamp");
}

TEST_CASE("outliner search: an entity stays with its matching brush only, and with all brushes when it matches itself") {
    const auto d = docOf({point("p1", "info_player_start"), point("l1", "light", "hall_lamp"), brushEnt("d1", "func_detail", {"d1a", "d1b"})});
    CHECK(tools::buildOutliner(*d, {}, {}, all({"d1"}, "d1b")).rows.size() == 2);
    CHECK(tools::buildOutliner(*d, {}, {}, all({}, "d1b")).rows.size() == 2);   // listed for its brush, so it is open though it is not expanded
    CHECK(tools::buildOutliner(*d, {}, {}, all({"d1"}, "func_detail")).rows.size() == 3);
}

TEST_CASE("outliner search: every word must match; says so when nothing does; says when the map is empty") {
    const auto d = docOf({point("p1", "info_player_start"), point("l1", "light", "hall_lamp")});
    const auto none = tools::buildOutliner(*d, {}, {}, all({}, "light info"));
    CHECK(none.rows.empty());
    CHECK(none.message.find("Nothing matches") != std::string::npos);
    CHECK(tools::buildOutliner(*d, {}, {}, all({}, "hall lamp")).rows.size() == 1);
    CHECK(tools::buildOutliner(*docOf({}), {}, {}, all({})).message.find("Empty map") != std::string::npos);
    CHECK(tools::buildOutliner(*d, {}, {}, all({})).message.empty());
}

TEST_CASE("outliner search: prefab instances are found by name, file and the word prefab") {
    core::PrefabInstance p;
    p.id = "pi";
    p.file = "prefabs/door.map.json";
    const auto d = docOf({point("p1", "info_player_start")}, {p});
    CHECK(keys(tools::buildOutliner(*d, {}, {}, all({}, "door"))) == std::vector<std::string>{"i:pi"});
    CHECK(tools::buildOutliner(*d, {}, {}, all({}, "prefab")).rows.size() == 1);
}

TEST_CASE("outliner fold: starts collapsed, entities only, with the brush count; a point entity has no fold arrow") {
    const auto d = docOf({brushEnt("d1", "func_detail", {"d1a", "d1b"}), point("p1", "info_player_start")});
    const auto m = tools::buildOutliner(*d, {}, {}, all({}));
    CHECK(keys(m) == std::vector<std::string>{"e:d1", "e:p1"});
    CHECK(row(m, "e:d1")->detail.find("2 brushes") != std::string::npos);
    CHECK(row(m, "e:d1")->foldable);
    CHECK_FALSE(row(m, "e:d1")->open);
    CHECK_FALSE(row(m, "e:p1")->foldable);
    const auto open = tools::buildOutliner(*d, {}, {}, all({"d1"}));
    CHECK(keys(open) == std::vector<std::string>{"e:d1", "b:d1|d1a", "b:d1|d1b", "e:p1"});
    CHECK(row(open, "e:d1")->detail.find("brushes") == std::string::npos);
}

TEST_CASE("outliner fold: a selected brush lights its collapsed entity and does not open it; open, the brush row is lit instead") {
    const auto d = docOf({brushEnt("d1", "func_detail", {"d1a", "d1b"}), point("p1", "info_player_start")});
    const std::vector<SelRef> sel = {SelRef::brush("d1", "d1b")};
    const auto folded = tools::buildOutliner(*d, sel, {}, all({}));
    CHECK(keys(folded) == std::vector<std::string>{"e:d1", "e:p1"});
    CHECK(row(folded, "e:d1")->selected);
    const auto open = tools::buildOutliner(*d, sel, {}, all({"d1"}));
    CHECK_FALSE(row(open, "e:d1")->selected);
    CHECK(row(open, "b:d1|d1b")->selected);
    CHECK_FALSE(row(open, "b:d1|d1a")->selected);
    // an entity selected as a whole is lit open or closed; a face selection lights its brush
    CHECK(row(tools::buildOutliner(*d, {SelRef::entity("d1")}, {}, all({"d1"})), "e:d1")->selected);
    CHECK(row(tools::buildOutliner(*d, {SelRef::face("d1", "d1a", 2)}, {}, all({"d1"})), "b:d1|d1a")->selected);
}

TEST_CASE("outliner eye: hide state of entities, brushes and prefab instances") {
    const auto d = docOf({brushEnt("g", "func_group", {"g1", "g2"}), brushEnt("t", "trigger_hurt", {"t1"}), point("p", "info_player_start")});
    tools::HideState none;
    const auto state = [&](const tools::HideState& h, const std::string& e, const std::string& cls, const std::string& b = {}) { return tools::eyeState(h, e, cls, b); };
    CHECK(state(none, "g", "func_group") == EyeState::No);

    tools::HideState self;
    self.keys.insert(tools::entityKey("g"));
    CHECK(state(self, "g", "func_group") == EyeState::Self);
    CHECK(state(self, "g", "func_group", "g1") == EyeState::Parent);

    tools::HideState trigs;
    trigs.rules = {{"trigger_*", true, false}};
    CHECK(state(trigs, "t", "trigger_hurt") == EyeState::Rule);
    CHECK(state(trigs, "t", "trigger_hurt", "t1") == EyeState::Parent);
    CHECK(state(trigs, "g", "func_group") == EyeState::No);

    tools::HideState brush;
    brush.keys.insert(tools::brushKey("g", "g1"));
    CHECK(state(brush, "g", "func_group", "g1") == EyeState::Self);
    CHECK(state(brush, "g", "func_group", "g2") == EyeState::No);
    CHECK(state(brush, "g", "func_group") == EyeState::No);

    CHECK(tools::eyeHidden(EyeState::Parent));
    CHECK(tools::eyeLocked(EyeState::Rule));
    CHECK_FALSE(tools::eyeLocked(EyeState::Self));
    CHECK_FALSE(tools::eyeHidden(EyeState::No));
}

TEST_CASE("outliner eye: every row carries its eye; hidden rows are marked; a prefab instance hides with its key") {
    core::PrefabInstance p;
    p.id = "pi";
    p.file = "p.json";
    const auto d = docOf({brushEnt("g", "func_group", {"g1", "g2"}), brushEnt("t", "trigger_hurt", {"t1"}), point("p", "info_player_start")}, {p});
    tools::HideState h;
    h.keys = {tools::brushKey("g", "g1"), tools::instanceKey("pi")};
    h.rules = {{"trigger_*", true, false}};
    const auto m = tools::buildOutliner(*d, {}, h, all({"g", "t"}));
    CHECK(keys(m) == std::vector<std::string>{"e:g", "b:g|g1", "b:g|g2", "e:t", "b:t|t1", "e:p", "i:pi"});
    CHECK(row(m, "b:g|g1")->eye == EyeState::Self);
    CHECK(row(m, "b:g|g2")->eye == EyeState::No);
    CHECK(row(m, "e:t")->eye == EyeState::Rule);
    CHECK(row(m, "b:t|t1")->eye == EyeState::Parent);
    CHECK(row(m, "i:pi")->eye == EyeState::Self);
    CHECK(row(m, "i:pi")->hidden());
    CHECK_FALSE(row(m, "e:p")->hidden());
}

TEST_CASE("outliner reveal: only a changed primary selection moves the list") {
    tools::RevealTracker t;
    const SelRef b25 = SelRef::brush("w", "b25");
    CHECK(t.update(&b25) == std::vector<std::string>{"b:w|b25", "e:w"});   // the brush row first, its entity's row when the brush is folded away
    CHECK(t.update(&b25).empty());                                           // the same selection: nothing moves
    t.update(nullptr);                                                       // clearing the selection does not scroll
    CHECK(t.update(nullptr).empty());
    CHECK(t.update(&b25) == std::vector<std::string>{"b:w|b25", "e:w"});    // selecting it again after it was deselected scrolls again
    const SelRef p1 = SelRef::entity("p1");
    CHECK(t.update(&p1) == std::vector<std::string>{"e:p1"});
}

TEST_CASE("outliner reveal: a face selection goes to its brush; a prefab instance to its row") {
    tools::RevealTracker t;
    const SelRef b2 = SelRef::brush("w", "b2"), b27 = SelRef::brush("w", "b27"), face = SelRef::face("w", "b10", 2), inst = SelRef::instance("pi");
    CHECK(t.update(&b27) == std::vector<std::string>{"b:w|b27", "e:w"});
    CHECK(t.update(&face) == std::vector<std::string>{"b:w|b10", "e:w"});
    CHECK(t.update(&inst) == std::vector<std::string>{"i:pi"});
    (void)b2;
}
