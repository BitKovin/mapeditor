// T11.10c: the right-click menus: the entries (pure), the registry actions they name, the selection that follows the click, and telling a click from a camera drag.
#include <doctest.h>

#include <QAction>
#include <QMenu>

#include "doc/edit.h"
#include "doc/io.h"
#include "render/picking.h"
#include "tools/context_menu.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {4, 2, 3}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {10, 0, 0}, {12, 2, 3}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

render::PickHit hitOn(const std::string& brush, uint32_t face) {
    render::PickHit h;
    h.entityId = "w";
    h.brushId = brush;
    h.faceId = face;
    return h;
}

QAction* find(QMenu& menu, const QString& action, const QString& text = {}) {
    for (QAction* a : menu.actions()) {
        if (a->menu())
            if (QAction* inner = find(*a->menu(), action, text)) return inner;
        if (a->objectName() == action && (text.isEmpty() || a->text() == text)) return a;
    }
    return nullptr;
}

size_t brushCount(ui::MainWindow& w, const std::string& entity) {
    return w.document()->entities[static_cast<size_t>(w.document()->findEntity(entity))]->brushes.size();
}

} // namespace

TEST_CASE("context menu: the object menu lists the Shell's entries, each naming a registry action") {
    const auto entries = tools::objectContextMenu({"func_door", "func_button", "func_door"}, 3);
    REQUIRE(entries.size() >= 20);
    CHECK(entries[0].label == "Make entity");
    REQUIRE(entries[0].sub.size() == 2);   // sorted, no duplicates
    CHECK(entries[0].sub[0].params["classname"] == "func_button");
    CHECK(entries[0].sub[0].action == "edit.make_entity");
    bool unhide = false, separators = false;
    for (const tools::MenuEntry& e : entries) {
        if (e.action == "edit.unhide_all") unhide = e.label == "Unhide all (3)";
        if (e.separator) separators = true;
    }
    CHECK(unhide);
    CHECK(separators);
    CHECK(tools::makeEntityClasses({}).size() == 4);   // without a definition list
    CHECK(tools::meshContextMenu().size() == 8);
    const auto face = tools::surfaceContextMenu("w", "a", 3);
    REQUIRE(face.size() == 2);
    CHECK(face[1].action == "context.flow_surface");
    CHECK(face[1].params["faceId"] == 3);
}

TEST_CASE("context menu: every action a menu names exists in the registry") {
    ui::MainWindow window(services());
    std::vector<tools::MenuEntry> all = tools::objectContextMenu({}, 0);
    for (const auto& e : tools::meshContextMenu()) all.push_back(e);
    for (const auto& e : tools::surfaceContextMenu("w", "a", 0)) all.push_back(e);
    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i].sub.empty()) all.insert(all.end(), all[i].sub.begin(), all[i].sub.end());
        if (all[i].separator || !all[i].sub.empty()) continue;
        INFO("action " << all[i].action);
        CHECK(window.registry().find(all[i].action) != nullptr);
    }
}

TEST_CASE("context menu: a right-click selects what is under it and the entries follow the selection") {
    ui::MainWindow window(services());
    window.setDocument(oneBox(), true);
    // nothing selected, nothing hit: only what needs no selection is enabled
    auto empty = window.contextMenuOf(window.contextEntries(std::nullopt), nullptr);
    CHECK_FALSE(find(*empty, "edit.delete")->isEnabled());
    CHECK_FALSE(find(*empty, "edit.unhide_all")->isEnabled());
    CHECK(find(*empty, "edit.paste")->isEnabled());
    // a hit on brush "a" selects it
    const auto entries = window.contextEntries(hitOn("a", 0));
    REQUIRE(window.selection().items().size() == 1);
    CHECK(window.selection().items()[0].brushId == "a");
    auto menu = window.contextMenuOf(entries, nullptr);
    CHECK(find(*menu, "edit.delete")->isEnabled());
    CHECK(find(*menu, "edit.make_entity", "func_detail")->isEnabled());
    // a hit on a brush that is part of the selection leaves the selection alone
    window.selection().selectMany(*window.document(), {doc::SelRef::brush("w", "a"), doc::SelRef::brush("w", "b")});
    (void)window.contextEntries(hitOn("a", 1));
    CHECK(window.selection().items().size() == 2);
}

TEST_CASE("context menu: Make entity and Delete run through the registry as one undo step each") {
    ui::MainWindow window(services());
    window.setDocument(oneBox(), true);
    auto menu = window.contextMenuOf(window.contextEntries(hitOn("a", 0)), nullptr);
    find(*menu, "edit.make_entity", "func_detail")->trigger();
    REQUIRE(window.document()->findEntity("w") >= 0);
    CHECK(brushCount(window, "w") == 1);   // brush "a" left the world
    CHECK(window.document()->entities.size() == 2);
    window.undo();
    CHECK(window.document()->entities.size() == 1);
    CHECK(brushCount(window, "w") == 2);
    auto again = window.contextMenuOf(window.contextEntries(hitOn("b", 0)), nullptr);
    find(*again, "edit.delete")->trigger();
    CHECK(brushCount(window, "w") == 1);
}

TEST_CASE("context menu: with a face selected, another face offers the surface menu") {
    ui::MainWindow window(services());
    window.setDocument(oneBox(), true);
    window.selection().select(hitOn("a", 0).toSelRef(true));
    REQUIRE(window.selectedFaceRef().has_value());
    const auto entries = window.contextEntries(hitOn("b", 2));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].action == "context.copy_surface");
    CHECK(entries[0].params["brushId"] == "b");
    CHECK(entries[0].params["faceId"] == 2);
    CHECK(window.selectedFaceRef().has_value());   // the selection stays so the paste can be repeated
    // the same face again is an ordinary right-click on it
    const auto same = window.contextEntries(hitOn("a", 0));
    CHECK(same.size() > 2);
}

TEST_CASE("context menu: a click is told from a drag by the pointer travel") {
    ui::MainWindow window(services());
    window.contextClickBegin(QPointF(40, 50));
    window.contextClickMove(1, -2);
    CHECK(window.contextClickEnd());
    CHECK(window.contextClickAt() == QPointF(40, 50));
    window.contextClickMove(3, 0);
    CHECK_FALSE(window.contextClickEnd());
    window.contextClickBegin(QPointF(1, 1));   // the next press starts over
    CHECK(window.contextClickEnd());
}
