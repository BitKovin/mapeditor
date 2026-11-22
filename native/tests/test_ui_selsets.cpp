// M11 item 11, U9: named selection sets (nine per map, view state kept next to the camera bookmarks), headless window.
#include <doctest.h>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> map(const std::string& path) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {4, 0, 0}, {6, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity p;
    p.id = "p";
    p.classname = "nav_point";
    REQUIRE(ed.addEntity(p));
    const auto v = ed.finish();
    REQUIRE(v);
    auto copy = std::make_shared<doc::DocumentVersion>(*v);
    copy->path = path;
    return copy;
}

} // namespace

TEST_CASE("selection sets: saved per map and slot, recalled as far as the items still exist") {
    ui::MainWindow window(services());
    window.setDocument(map("maps/one.map.json"), true);
    CHECK_FALSE(window.hasSelectionSet(1));
    CHECK_FALSE(window.saveSelectionSet(1));   // nothing selected
    CHECK_FALSE(window.recallSelectionSet(1));
    CHECK(window.registry().find("edit.selection_set.save") != nullptr);

    window.selection().selectMany(*window.document(), {doc::SelRef::brush("w", "a"), doc::SelRef::entity("p")});
    CHECK(window.runAction("edit.selection_set.save", {{"slot", 2}}));
    CHECK(window.hasSelectionSet(2));
    CHECK_FALSE(window.hasSelectionSet(1));
    window.selection().clear();
    CHECK(window.runAction("edit.selection_set.recall", {{"slot", 2}}));
    REQUIRE(window.selection().items().size() == 2);
    CHECK(window.selection().contains(doc::SelRef::brush("w", "a")));
    CHECK(window.selection().contains(doc::SelRef::entity("p")));

    // a face and a prefab instance survive the round trip of the stored text
    window.selection().select(doc::SelRef::face("w", "b", 3));
    CHECK(window.saveSelectionSet(3));
    window.selection().clear();
    CHECK(window.recallSelectionSet(3));
    REQUIRE(window.selection().primary());
    CHECK(window.selection().primary()->faceId == std::optional<uint32_t>(3));

    // another map has its own sets
    window.setDocument(map("maps/two.map.json"), true);
    CHECK_FALSE(window.hasSelectionSet(2));

    // what was deleted since is left out; a set of nothing that exists selects nothing
    window.setDocument(map("maps/one.map.json"), true);
    REQUIRE(window.hasSelectionSet(2));
    doc::VersionEditor ed(window.document());
    REQUIRE(ed.removeEntity("p"));
    window.setDocument(ed.finish(), true);
    window.selection().clear();
    CHECK(window.recallSelectionSet(2));
    REQUIRE(window.selection().items().size() == 1);
    CHECK(window.selection().primary()->brushId == "a");
    CHECK_FALSE(window.hasSelectionSet(9));
}
