// T11.1f (Qt side): mesh mode in the main window: entering it, the element kind, the instant tools as undo steps, a concave result moving into func_detail, the keys and
// the menu. Offscreen platform, the window is never shown, no GPU (the picking in a viewport needs one and is covered by the pure tests of tools/mesh/pick).
#include <doctest.h>

#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;
namespace msh = native::tools::mesh;

namespace {

constexpr uint32_t TOP = 5;   // makeBox: face 5 is +Y

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

struct Fixture {
    ui::MainWindow window;
    Fixture() : window(services()) { window.setDocument(oneBox(), true); }
    const core::Brush& brush() { return window.document()->entities[0]->brushes[0]->brush; }
    void selectBrush() { window.selection().select(SelRef::brush("w", "b")); }
    bool enter() {
        selectBrush();
        return window.enterMeshMode();
    }
    // a key press as the router sees it, with the contexts the window would set in a viewport
    bool key(const std::string& name, uint8_t mods = 0) {
        std::vector<std::string> contexts = {"window:main", "area:viewport", "view:perspective", "view:fly", "tool:select"};
        if (window.meshMode()) {
            contexts.push_back("mode:mesh");
            contexts.push_back("mode:mesh.vertex");
        }
        window.router().contexts().set(contexts);
        ui::UiEnv env{&window, nullptr};
        actions::ActionEnv ae{&env};
        actions::InputEvent ev;
        ev.type = actions::InputEvent::Type::KeyPress;
        ev.key = name;
        ev.scan = name;
        ev.mods = mods;
        const bool handled = window.router().handle(ev, ae).handled;
        ev.type = actions::InputEvent::Type::KeyRelease;
        window.router().handle(ev, ae);
        return handled;
    }
};

// a pit in the top face: inset it, then push the inner face into the brush (concave)
msh::OpResult pit(const core::Brush& b, const msh::MeshSel&) {
    msh::OpResult in = msh::insetFaces(b, {TOP}, 0.5);
    if (!in.ok()) return in;
    return msh::extrudeFaces(in.brush, in.sel.faces, -0.5);
}

} // namespace

TEST_CASE("mesh mode: Tab enters it with a brush selected and leaves it again; nothing selected is refused with a reason") {
    Fixture f;
    CHECK_FALSE(f.window.enterMeshMode());   // nothing selected
    CHECK_FALSE(f.window.meshMode());
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Select a brush")));
    f.selectBrush();
    CHECK(f.window.runAction("mesh.toggle"));
    CHECK(f.window.meshMode());
    REQUIRE(f.window.meshTarget());
    CHECK(f.window.meshTarget()->brushId == "b");
    CHECK(f.window.modeContext() == "mode:mesh");
    CHECK(f.window.runAction("mesh.toggle"));
    CHECK_FALSE(f.window.meshMode());
    CHECK_FALSE(f.window.meshTarget().has_value());
}

TEST_CASE("mesh mode: an entity of one brush is edited through its brush; the Box tool ends the mode") {
    Fixture f;
    core::Entity d;
    d.id = "d";
    d.classname = "func_detail";
    d.brushes.push_back(core::Brush::makeBox("db", {5, 0, 0}, {6, 1, 1}, "dev/grey"));
    doc::VersionEditor ed(f.window.document());
    REQUIRE(ed.addEntity(d));
    f.window.setDocument(ed.finish(), false);
    f.window.selection().select(SelRef::entity("d"));
    REQUIRE(f.window.enterMeshMode());
    CHECK(f.window.selection().primary()->brushId == "db");
    f.window.setTool(ui::MainWindow::Tool::Box);
    CHECK_FALSE(f.window.meshMode());
}

TEST_CASE("mesh mode: the element kind converts what is selected") {
    Fixture f;
    REQUIRE(f.enter());
    CHECK(f.window.meshElement() == msh::SelMode::Vertex);
    msh::MeshSel s;
    s.verts = {0, 1};
    f.window.setMeshSelection(s);
    CHECK(f.window.runAction("mesh.element", {{"element", "edge"}}));
    CHECK(f.window.meshElement() == msh::SelMode::Edge);
    REQUIRE(f.window.meshSelection().edges.size() == 1);
    CHECK(f.window.meshSelection().edges[0] == msh::edgeKey(0, 1));
    CHECK(f.window.runAction("mesh.element", {{"element", "face"}}));
    CHECK(f.window.meshSelection().faces.empty());   // one edge encloses no face (the picked edge itself stays picked)
}

TEST_CASE("mesh mode: the selection tools") {
    Fixture f;
    REQUIRE(f.enter());
    CHECK(f.window.runAction("mesh.select_all"));
    CHECK(f.window.meshSelection().verts.size() == 8);
    CHECK(f.window.runAction("mesh.select_none"));
    CHECK(f.window.meshSelection().empty());
    msh::MeshSel one;
    one.verts = {0};
    f.window.setMeshSelection(one);
    CHECK(f.window.runAction("mesh.select_grow"));
    CHECK(f.window.meshSelection().verts.size() == 4);
    CHECK(f.window.runAction("mesh.select_shrink"));
    CHECK(f.window.meshSelection().verts.size() == 1);
    CHECK(f.window.runAction("mesh.select_invert"));
    CHECK(f.window.meshSelection().verts.size() == 7);
}

TEST_CASE("mesh mode: an instant tool is one undo step and keeps the mode") {
    Fixture f;
    REQUIRE(f.enter());
    msh::MeshSel s;
    s.verts = {0, 2};   // opposite corners of the bottom face
    f.window.setMeshSelection(s);
    CHECK(f.window.runAction("mesh.connect"));
    CHECK(f.brush().faces.size() == 7);
    CHECK(f.window.meshMode());
    CHECK(f.window.meshSelection().edges.size() == 1);   // the new edge
    CHECK(f.window.undoText() == "Undo Connect vertices");
    f.window.undo();
    CHECK(f.brush().faces.size() == 6);
    CHECK(f.window.meshMode());
    f.window.redo();
    CHECK(f.brush().faces.size() == 7);
}

TEST_CASE("mesh mode: a tool that has nothing to work on says why and changes nothing") {
    Fixture f;
    REQUIRE(f.enter());
    const uint64_t before = f.window.document()->version;
    f.window.runAction("mesh.connect");   // nothing selected: the action runs, the tool refuses
    CHECK(f.window.document()->version == before);
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Connect")));
}

TEST_CASE("mesh mode: deleting a face leaves the brush open, so it moves into func_detail; Fill closes it again") {
    Fixture f;
    REQUIRE(f.enter());
    CHECK(f.window.runAction("mesh.element", {{"element", "face"}}));
    msh::MeshSel s;
    s.faces = {TOP};
    f.window.setMeshSelection(s);
    f.window.runAction("mesh.delete");
    REQUIRE(f.window.document()->entities.size() == 2);   // worldspawn may only hold closed convex solids
    const core::Brush& open = f.window.document()->entities[1]->brushes[0]->brush;
    CHECK(open.faces.size() == 5);
    CHECK(f.window.meshMode());
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("func_detail")));
    // the open rim is picked in vertex mode and filled
    CHECK(f.window.runAction("mesh.element", {{"element", "vertex"}}));
    CHECK(f.window.runAction("mesh.select_all"));
    CHECK(f.window.runAction("mesh.fill"));
    CHECK(f.window.document()->entities[1]->brushes[0]->brush.faces.size() == 6);
    CHECK(msh::isClosedManifold(f.window.document()->entities[1]->brushes[0]->brush));
}

TEST_CASE("mesh mode: a concave result moves into a new func_detail entity that stays selected for editing") {
    Fixture f;
    REQUIRE(f.enter());
    CHECK(f.window.runAction("mesh.element", {{"element", "face"}}));
    msh::MeshSel s;
    s.faces = {TOP};
    f.window.setMeshSelection(s);
    REQUIRE(f.window.applyMeshOperation("Pit", pit));
    REQUIRE(f.window.document()->entities.size() == 2);
    CHECK(f.window.document()->entities[0]->brushes.empty());
    CHECK(f.window.document()->entities[1]->typed.classname == "func_detail");
    CHECK(f.window.meshMode());
    REQUIRE(f.window.selection().primary());
    CHECK(f.window.selection().primary()->entityId == f.window.document()->entities[1]->typed.id);
    CHECK(f.window.selection().primary()->brushId == "b");
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("func_detail")));
    f.window.undo();
    REQUIRE(f.window.document()->entities.size() == 1);   // the entity is gone again
    CHECK(f.window.document()->entities[0]->brushes.size() == 1);
}

TEST_CASE("mesh mode: the keys of the default keymap") {
    Fixture f;
    f.selectBrush();
    CHECK(f.key("Tab"));
    CHECK(f.window.meshMode());
    CHECK(f.key("3"));
    CHECK(f.window.meshElement() == msh::SelMode::Face);
    CHECK(f.key("2"));
    CHECK(f.window.meshElement() == msh::SelMode::Edge);
    CHECK(f.key("1"));
    CHECK(f.window.meshElement() == msh::SelMode::Vertex);
    CHECK(f.key("A"));
    CHECK(f.window.meshSelection().verts.size() == 8);
    CHECK(f.key("A", actions::kAlt));
    CHECK(f.window.meshSelection().empty());
    // Delete in mesh mode is the mesh tool, never the object delete: with nothing picked the brush stays
    f.key("Delete");
    CHECK(f.window.document()->entities[0]->brushes.size() == 1);
    CHECK(f.brush().faces.size() == 6);
    // J connects: pick two corners of the bottom face
    msh::MeshSel s;
    s.verts = {0, 2};
    f.window.setMeshSelection(s);
    CHECK(f.key("J"));
    CHECK(f.brush().faces.size() == 7);
    CHECK(f.key("Tab"));
    CHECK_FALSE(f.window.meshMode());
    // outside mesh mode the same keys are not the mesh tools: Delete is the object delete again
    f.selectBrush();
    f.key("Delete");
    CHECK(f.window.document()->entities[0]->brushes.empty());
}

TEST_CASE("mesh mode: the overlay lines show every edge, the picked ones in another colour") {
    Fixture f;
    CHECK(f.window.meshEdgeLines().empty());   // not in mesh mode
    REQUIRE(f.enter());
    const auto plain = f.window.meshEdgeLines();
    CHECK(plain.size() == 24);   // 12 edges, two vertices each
    msh::MeshSel s;
    s.edges = {msh::edgeKey(0, 1)};
    CHECK(f.window.runAction("mesh.element", {{"element", "edge"}}));
    f.window.setMeshSelection(s);
    const auto lit = f.window.meshEdgeLines();
    REQUIRE(lit.size() == 24);
    size_t picked = 0;
    for (size_t i = 0; i < lit.size(); i += 2)
        if (lit[i].rgba != plain[i].rgba) ++picked;
    CHECK(picked == 1);
}

TEST_CASE("mesh mode: the Mesh menu lists the tools with their keys") {
    Fixture f;
    QMenu* mesh = nullptr;
    for (QAction* a : f.window.menuBar()->actions())
        if (a->menu() && a->text().remove('&') == "Mesh") mesh = a->menu();
    REQUIRE(mesh);
    QStringList texts;
    for (QAction* a : mesh->actions()) texts << a->text();
    const QString all = texts.join('|');
    CHECK(all.contains(QStringLiteral("Edit mesh")));
    CHECK(all.contains(QStringLiteral("Connect vertices")));
    CHECK(all.contains(QStringLiteral("Dissolve")));
    CHECK(f.window.keymap().displayText("mesh.connect") == "J");
    CHECK(f.window.keymap().displayText("mesh.toggle") == "Tab");
}
