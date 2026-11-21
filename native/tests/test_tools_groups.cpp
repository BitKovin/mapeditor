// T11.4b: the pure helpers of the texture group editor (templates, names, slot edits, parameter definitions, preview geometry) and the group edits of faces (apply, edit layout, clear).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/texturing/face_edit.h"
#include "tools/texturing/group_tools.h"

using namespace native;
using namespace native::tools::tex;
using doc::SelRef;
using native::tools::BrushEdit;
namespace tp = core::texplan;
using ojson = nlohmann::ordered_json;

namespace {

const tp::TexInfo* fakeInfo(const std::string& path) {
    static std::map<std::string, tp::TexInfo> m;
    auto it = m.find(path);
    if (it == m.end()) {
        tp::TexInfo i;
        i.path = path;
        i.width = 256;
        i.height = 128;
        i.ppm = 64;
        i.props = tp::defaultProps();
        it = m.emplace(path, i).first;
    }
    return &it->second;
}

std::shared_ptr<const doc::DocumentVersion> makeDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b1", {0, 0, 0}, {4, 2, 1}, "dev/wall"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

const core::Brush& brushOf(const doc::DocumentVersion& d, const std::string& id) {
    for (const auto& n : d.entities[0]->brushes)
        if (n->brush.id == id) return n->brush;
    throw std::runtime_error("no brush");
}

int withLayout(const doc::DocumentVersion& d, const std::string& group) {
    int n = 0;
    for (const core::BrushFace& f : brushOf(d, "b1").faces)
        if (layoutGroupOf(f) == group) ++n;
    return n;
}

} // namespace

TEST_CASE("group names: letters, digits, _ and - with / for folders") {
    CHECK(validGroupName("facade"));
    CHECK(validGroupName("buildings/apartment_block-2"));
    CHECK_FALSE(validGroupName(""));
    CHECK_FALSE(validGroupName("/a"));
    CHECK_FALSE(validGroupName("a/"));
    CHECK_FALSE(validGroupName("a//b"));
    CHECK_FALSE(validGroupName("a b"));
    CHECK_FALSE(validGroupName("a.b"));
    CHECK_FALSE(validGroupName("../a"));
    CHECK_FALSE(validGroupName(std::string(121, 'a')));
}

TEST_CASE("group templates: every template is a valid group that lays out a wall and a floor") {
    REQUIRE(groupTemplates().size() == 3);
    for (const GroupTemplate& t : groupTemplates()) {
        INFO(t.id);
        const ojson j = ojson::parse(t.text, nullptr, false);
        REQUIRE_FALSE(j.is_discarded());
        const tp::GroupParse p = tp::parseGroup(j);
        CHECK(p.errors.empty());
        REQUIRE(p.group);
        CHECK_FALSE(paramDefsOf(j).empty());
        CHECK_FALSE(slotsOf(j).empty());
        for (const std::string facing : {"wall", "up"}) {
            tp::LayoutInput in;
            in.group = p.group.get();
            in.w = 9;
            in.h = 12;
            in.facing = facing;
            in.tilt = facing == "up" ? 0 : 90;
            in.info = [](const std::string& s) { return fakeInfo(s); };
            const tp::LayoutResult r = tp::solveLayout(in);
            CHECK_FALSE(r.cells.empty());
            CHECK_FALSE(r.truncated);
        }
    }
}

TEST_CASE("slots: both forms are read and changed, other text is kept in order") {
    const std::string text = R"({"slots": {"b": "x/b", "a": {"texture": "x/a", "note": 1}}, "root": {"tex": "a"}})";
    const auto slots = slotsOf(ojson::parse(text));
    REQUIRE(slots.size() == 2);
    CHECK(slots[0] == std::make_pair(std::string("b"), std::string("x/b")));   // file order
    CHECK(slots[1] == std::make_pair(std::string("a"), std::string("x/a")));
    const auto next = setSlotTexture(text, "a", "y/new");
    REQUIRE(next);
    const ojson j = ojson::parse(*next);
    CHECK(j["slots"]["a"]["texture"] == "y/new");
    CHECK(j["slots"]["a"]["note"] == 1);
    CHECK(j["slots"].begin().key() == "b");   // order kept
    const auto plain = setSlotTexture(text, "b", "y/b");
    REQUIRE(plain);
    CHECK(ojson::parse(*plain)["slots"]["b"] == "y/b");
    CHECK_FALSE(setSlotTexture(text, "nope", "y/b"));
    CHECK_FALSE(setSlotTexture("{ not json", "a", "y/b"));
    CHECK_FALSE(setSlotTexture(R"({"root": {}})", "a", "y/b"));
}

TEST_CASE("parameters: the definitions are read with their limits and defaults; unusable ones are left out") {
    const ojson g = ojson::parse(R"({"params": {
        "h": {"type": "number", "default": 99, "min": 2, "max": 6, "step": 0.5, "label": "Height"},
        "n": {"type": "int", "default": 2.6, "min": 1, "max": 9},
        "on": {"type": "bool", "default": true},
        "style": {"type": "choice", "options": ["a", "b"], "default": "zzz"},
        "empty": {"type": "choice", "options": []},
        "odd": {"type": "vector"},
        "bad": 5}})");
    const auto p = paramDefsOf(g);
    REQUIRE(p.size() == 4);
    CHECK(p[0].name == "h");
    CHECK(p[0].label == "Height");
    CHECK(p[0].defNumber == doctest::Approx(6));   // clamped into the range
    CHECK(p[0].step == doctest::Approx(0.5));
    CHECK(p[1].type == "int");
    CHECK(p[1].defNumber == doctest::Approx(3));
    CHECK(p[1].label == "n");
    CHECK(p[2].defBool);
    CHECK(p[3].defChoice == "a");   // an unknown default falls back to the first option
    CHECK(paramDefsOf(ojson::parse("[]")).empty());
}

TEST_CASE("preview geometry: the scale fits the face, the cell under a point is the later one on shared edges") {
    CHECK(previewScale(520, 440, 10, 20, 8) == doctest::Approx(424.0 / 20));
    CHECK(previewScale(520, 440, 40, 5, 8) == doctest::Approx(504.0 / 40));
    std::vector<tp::Cell> cells(2);
    cells[0].x0 = 0;
    cells[0].x1 = 2;
    cells[0].y0 = 0;
    cells[0].y1 = 3;
    cells[1].x0 = 2;
    cells[1].x1 = 5;
    cells[1].y0 = 0;
    cells[1].y1 = 3;
    CHECK(cellAt(cells, 1, 1) == 0);
    CHECK(cellAt(cells, 3, 1) == 1);
    CHECK(cellAt(cells, 2, 1) == 1);
    CHECK(cellAt(cells, 6, 1) == -1);
    CHECK(cellAt(cells, 1, -0.5) == -1);
}

TEST_CASE("preview geometry: a stretched cell is drawn as large as the face on the canvas") {
    const tp::TexInfo& ti = *fakeInfo("dev/a");
    tp::Cell c;
    c.x0 = 0;
    c.y0 = 0;
    c.x1 = 4;
    c.y1 = 2;
    c.fit = "stretch";
    const double scale = 50;
    const auto m = previewPatternMatrix(tp::cellMap(c, tp::naturalSize(ti), 4, 2), ti.width, ti.height, scale, 2);
    // one repeat of the picture is the whole cell: the image's width maps to 4 m and its height to 2 m
    CHECK(std::hypot(m[0], m[1]) * ti.width == doctest::Approx(scale * 4));
    CHECK(std::hypot(m[2], m[3]) * ti.height == doctest::Approx(scale * 2));
}

TEST_CASE("apply texture group: the faces get the layout and the fallback texture; walls only leaves the roof and floor alone") {
    const auto d = makeDoc();
    const std::vector<SelRef> sel = {SelRef::brush("w", "b1")};
    const BrushEdit all = applyTexGroup(d, sel, "bld/facade", "dev/metal", false);
    REQUIRE(all.ok());
    CHECK(all.message == "Texture group applied to 6 faces");
    CHECK(withLayout(*all.version, "bld/facade") == 6);
    for (const core::BrushFace& f : brushOf(*all.version, "b1").faces) CHECK(f.texture == "dev/metal");
    const BrushEdit walls = applyTexGroup(d, sel, "bld/set", "", true);
    REQUIRE(walls.ok());
    CHECK(withLayout(*walls.version, "bld/set") == 4);
    // a face picked by hand always gets it, also a roof
    int up = -1;
    for (const core::BrushFace& f : brushOf(*d, "b1").faces)
        if (std::abs(tp::faceNormalOf(brushOf(*d, "b1"), f)[1]) > 0.5) up = static_cast<int>(f.id);
    REQUIRE(up >= 0);
    const BrushEdit one = applyTexGroup(d, {SelRef::face("w", "b1", static_cast<uint32_t>(up))}, "bld/set", "", true);
    REQUIRE(one.ok());
    CHECK(withLayout(*one.version, "bld/set") == 1);
    CHECK(one.message == "Texture group applied to 1 face");
    // nothing selected: no edit
    CHECK_FALSE(applyTexGroup(d, {}, "bld/set", "", false).ok());
}

TEST_CASE("edit layout and clear layout: only faces that have a layout change; an empty result removes it") {
    const auto d = makeDoc();
    const std::vector<SelRef> sel = {SelRef::brush("w", "b1")};
    const uint32_t first = brushOf(*d, "b1").faces[0].id;
    const BrushEdit a = applyTexGroup(d, {SelRef::face("w", "b1", first)}, "bld/facade", "", false);
    REQUIRE(a.ok());
    const BrushEdit e = editLayout(a.version, sel, "Set seed", [](const nlohmann::json& l) {
        nlohmann::json n = l;
        n["seed"] = 7;
        return n;
    });
    REQUIRE(e.ok());
    int seeded = 0;
    for (const core::BrushFace& f : brushOf(*e.version, "b1").faces)
        if (f.look && f.look->contains("layout") && (*f.look)["layout"].contains("seed")) ++seeded;
    CHECK(seeded == 1);   // the other five faces have no layout and stay as they are
    CHECK(layoutGroupOf(*std::find_if(brushOf(*e.version, "b1").faces.begin(), brushOf(*e.version, "b1").faces.end(), [first](const core::BrushFace& f) { return f.id == first; })) == "bld/facade");
    const BrushEdit cleared = clearLayout(e.version, sel);
    REQUIRE(cleared.ok());
    CHECK(withLayout(*cleared.version, "bld/facade") == 0);
    // clearing a brush that has no layout is no edit
    CHECK_FALSE(clearLayout(cleared.version, sel).ok());
    CHECK(layoutGroupOf(core::BrushFace{}) == "");
}

TEST_CASE("groups panel model: the tab follows the kit key, the filter ignores case, copies get the first free name") {
    CHECK(groupTabOf(nlohmann::json::parse(R"({"kit": {"kind": "wall"}})")) == GroupTab::Simple);
    CHECK(groupTabOf(nlohmann::json::parse(R"({"root": {}})")) == GroupTab::Advanced);
    CHECK(groupTabOf(nlohmann::json::parse("[]")) == GroupTab::Advanced);
    const std::vector<std::string> names = {"bld/Facade", "bld/panels", "floor/tiles"};
    CHECK(filterGroups(names, "").size() == 3);
    CHECK(filterGroups(names, "FAC") == std::vector<std::string>{"bld/Facade"});
    CHECK(filterGroups(names, "bld").size() == 2);
    CHECK(filterGroups(names, "zzz").empty());
    CHECK(copyName("a", {"a"}) == "a_copy");
    CHECK(copyName("a", {"a", "a_copy"}) == "a_copy2");
    CHECK(copyName("a", {"a", "a_copy", "a_copy2"}) == "a_copy3");
}
