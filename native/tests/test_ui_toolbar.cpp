// M14 T14.3: the main tool bar (headless window): the buttons run the registry actions, follow their state, show the binding in the tooltip.
#include <doctest.h>

#include <QAction>
#include <QComboBox>
#include <QToolButton>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "ui/main_toolbar.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> brushMap() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

} // namespace

TEST_CASE("toolbar: it exists, every frequent action has a button, the tooltips carry the binding") {
    ui::MainWindow window(services());
    ui::MainToolBar* tb = window.toolbar();
    REQUIRE(tb);
    for (const char* id : {"file.save", "edit.undo", "edit.redo", "brush.clipper", "view.measure", "snap.toggle", "texture.uv_lock", "file.play"}) {
        INFO(id);
        CHECK(tb->find(id) != nullptr);
    }
    CHECK(tb->find("tool.set", {{"tool", "select"}}) != nullptr);
    CHECK(tb->find("tool.set", {{"tool", "box"}}) != nullptr);
    CHECK(tb->find("tool.set", {{"tool", "decal"}}) != nullptr);
    for (const char* m : {"move", "rotate", "scale"}) CHECK(tb->find("gizmo.mode", {{"mode", m}}) != nullptr);
    for (int n : {1, 2, 4}) CHECK(tb->find("view.layout", {{"count", n}}) != nullptr);
    CHECK(tb->find("file.save")->toolTip().contains("Ctrl+S"));
    CHECK(tb->find("edit.undo")->toolTip().contains("Ctrl+Z"));
    CHECK(tb->find("tool.set", {{"tool", "select"}})->toolTip().contains("Select"));
    CHECK_FALSE(tb->find("file.save")->icon().isNull());
    CHECK(tb->gridCombo()->count() > 3);
    CHECK(tb->rotationCombo()->count() > 3);
}

TEST_CASE("toolbar: the tool buttons switch the tool and show which one is active") {
    ui::MainWindow window(services());
    window.setDocument(brushMap(), true);
    ui::MainToolBar* tb = window.toolbar();
    QAction* select = tb->find("tool.set", {{"tool", "select"}});
    QAction* box = tb->find("tool.set", {{"tool", "box"}});
    QAction* decal = tb->find("tool.set", {{"tool", "decal"}});
    CHECK(select->isChecked());
    CHECK_FALSE(box->isChecked());
    box->trigger();
    CHECK(window.tool() == ui::MainWindow::Tool::Box);
    CHECK(box->isChecked());
    CHECK_FALSE(select->isChecked());
    decal->trigger();
    CHECK(window.tool() == ui::MainWindow::Tool::Decal);
    CHECK(decal->isChecked());
    select->trigger();
    CHECK(window.tool() == ui::MainWindow::Tool::Select);
    CHECK(select->isChecked());
    // a key (or the menu) changing the tool is shown on the bar too
    window.runAction("tool.set", {{"tool", "box"}});
    CHECK(box->isChecked());
    CHECK_FALSE(select->isChecked());
}

TEST_CASE("toolbar: Object | Mesh follows the edit mode, the element buttons only exist in mesh mode") {
    ui::MainWindow window(services());
    window.setDocument(brushMap(), true);
    ui::MainToolBar* tb = window.toolbar();
    CHECK(tb->objectModeAction()->isChecked());
    CHECK_FALSE(tb->meshModeAction()->isChecked());
    for (QAction* a : tb->elementActions()) CHECK_FALSE(a->isVisible());
    window.selection().selectMany(*window.document(), {doc::SelRef::brush("w", "a")});
    tb->meshModeAction()->trigger();
    CHECK(window.meshMode());
    CHECK(tb->meshModeAction()->isChecked());
    CHECK_FALSE(tb->objectModeAction()->isChecked());
    REQUIRE(tb->elementActions().size() == 3);
    for (QAction* a : tb->elementActions()) CHECK(a->isVisible());
    CHECK(tb->find("mesh.element", {{"element", "vertex"}})->isChecked());
    tb->find("mesh.element", {{"element", "face"}})->trigger();
    CHECK(tb->find("mesh.element", {{"element", "face"}})->isChecked());
    CHECK_FALSE(tb->find("mesh.element", {{"element", "vertex"}})->isChecked());
    tb->objectModeAction()->trigger();
    CHECK_FALSE(window.meshMode());
    for (QAction* a : tb->elementActions()) CHECK_FALSE(a->isVisible());
}

TEST_CASE("toolbar: undo and redo are enabled by the history, UV lock is a toggle that the window keeps") {
    ui::MainWindow window(services());
    window.setDocument(brushMap(), true);
    ui::MainToolBar* tb = window.toolbar();
    QAction* undo = tb->find("edit.undo");
    QAction* redo = tb->find("edit.redo");
    CHECK_FALSE(undo->isEnabled());
    CHECK_FALSE(redo->isEnabled());
    window.selection().selectMany(*window.document(), {doc::SelRef::brush("w", "a")});
    REQUIRE(window.runAction("edit.duplicate"));
    CHECK(undo->isEnabled());
    window.runAction("edit.undo");
    CHECK(redo->isEnabled());

    QAction* lock = tb->find("texture.uv_lock");
    CHECK(window.uvLock());
    CHECK(lock->isChecked());
    lock->trigger();
    CHECK_FALSE(window.uvLock());
    CHECK_FALSE(lock->isChecked());
    window.runAction("texture.uv_lock");
    CHECK(window.uvLock());
    CHECK(lock->isChecked());
}

TEST_CASE("UV lock: on, moving a brush bakes the texture coordinates it had onto the faces; off, the texture follows the world projection") {
    ui::MainWindow window(services());
    window.setDocument(brushMap(), true);
    const auto bakedFaces = [&] {
        int n = 0;
        for (const auto& f : window.document()->entities[0]->brushes[0]->brush.faces)
            if (!f.uvs.empty() && f.uvs.size() == f.loop.size()) ++n;
        return n;
    };
    window.selection().selectMany(*window.document(), {doc::SelRef::brush("w", "a")});
    CHECK(bakedFaces() == 0);
    window.setUvLock(false);
    REQUIRE(window.runAction("edit.nudge", {{"key", "right"}}));
    CHECK(bakedFaces() == 0);   // nothing was stored: the faces keep projecting from the world
    window.runAction("edit.undo");
    window.setUvLock(true);
    REQUIRE(window.runAction("edit.nudge", {{"key", "right"}}));
    CHECK(bakedFaces() == 6);   // every face carries the coordinates it had before the move
}
