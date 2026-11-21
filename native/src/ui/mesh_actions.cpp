// ui/mesh_actions -- see mesh_actions.h.
#include "ui/mesh_actions.h"

#include <string>

#include "ui/app_actions.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

using actions::ActionDef;
using actions::ActionEnv;
using actions::ActionKind;
using actions::Invocation;
using json = nlohmann::json;

MainWindow* windowOf(const ActionEnv& env) {
    const UiEnv* u = env.as<UiEnv>();
    return u ? u->window : nullptr;
}

bool inMeshMode(const ActionEnv& env, const json&) {
    const MainWindow* w = windowOf(env);
    return w && w->meshMode();
}

tools::mesh::SelMode elementOf(const json& p) {
    const std::string e = p.is_object() ? p.value("element", std::string("vertex")) : std::string("vertex");
    return e == "edge" ? tools::mesh::SelMode::Edge : e == "face" ? tools::mesh::SelMode::Face : tools::mesh::SelMode::Vertex;
}

struct ToolDef {
    const char* id;            // the action id: mesh.<id>
    const char* tool;          // the id tools/mesh/session knows
    const char* label;
    const char* description;
    const char* menu;
    bool separator;
};

void addTool(actions::Registry& r, const ToolDef& t) {
    ActionDef d;
    d.id = std::string("mesh.") + t.id;
    d.label = t.label;
    d.category = "Mesh";
    d.description = t.description;
    d.menu = t.menu;
    d.menuSeparatorBefore = t.separator;
    d.contexts = {"mode:mesh"};
    d.enabled = inMeshMode;
    const std::string tool = t.tool;
    d.run = [tool](ActionEnv& env, const Invocation&) {
        if (auto* w = windowOf(env)) w->runMeshTool(tool);
    };
    r.add(std::move(d));
}

} // namespace

void registerMeshActions(actions::Registry& r) {
    {
        ActionDef d;
        d.id = "mesh.toggle";
        d.label = "Edit mesh";
        d.category = "Mesh";
        d.description = "Switches the selected brush into mesh mode (its vertices, edges and faces) and back.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Mesh";
        d.contexts = {"area:viewport"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->document() && (w->meshMode() || !w->selection().empty());
        };
        d.checked = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->meshMode();
        };
        d.hint = "mesh mode";
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->toggleMeshMode();
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.element";
        d.label = "Select element";
        d.category = "Mesh";
        d.description = "Vertex, edge or face selection in mesh mode; what is selected carries over by Blender's rules.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Mesh/&Select";
        d.contexts = {"mode:mesh"};
        d.params = json{{"element", "vertex"}};
        d.variants = {{"&Vertices", {{"element", "vertex"}}}, {"&Edges", {{"element", "edge"}}}, {"&Faces", {{"element", "face"}}}};
        d.enabled = inMeshMode;
        d.checked = [](const ActionEnv& env, const json& p) {
            const MainWindow* w = windowOf(env);
            return w && w->meshMode() && w->meshElement() == elementOf(p);
        };
        d.hint = "element";
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->setMeshElement(elementOf(inv.params));
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.pick";
        d.label = "Select elements (click / box)";
        d.category = "Mesh";
        d.description = "Click selects the element under the cursor, Shift toggles it, Ctrl adds the shortest path from the last one, Alt selects its loop, Ctrl+Alt its ring; dragging a box selects what lies inside.";
        d.kind = ActionKind::Hold;
        d.contexts = {"mode:mesh"};
        d.hint = "select";
        d.enabled = [](const ActionEnv& env, const json&) {
            const UiEnv* u = env.as<UiEnv>();
            return u && u->window && u->viewport && u->window->meshMode();
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            const UiEnv* u = env.as<UiEnv>();
            if (u && u->window && u->viewport) u->window->meshPickInput(u->viewport, inv.event);
        };
        r.add(std::move(d));
    }

    const ToolDef tools[] = {
        {"connect", "connect", "Connect vertices", "Cuts the faces that hold two or more selected vertices between them.", "&Mesh", true},
        {"merge", "merge", "Merge vertices", "Welds the selected vertices into one at their centre.", "&Mesh", false},
        {"fill", "fill", "Fill hole", "Makes faces from the open edges among the selected vertices.", "&Mesh", false},
        {"dissolve", "dissolve", "Dissolve", "Removes the selected vertices, edges or faces and merges what was around them.", "&Mesh", false},
        {"delete", "delete", "Delete faces", "Deletes the faces that touch the selection. The brush stays open until filled, so it lives in a func_detail entity meanwhile.", "&Mesh", false},
        {"flip", "flip", "Flip normals", "Turns the selected faces around.", "&Mesh", true},
        {"recalc", "recalc", "Recalculate normals", "Makes the faces consistent and points them outward.", "&Mesh", false},
        {"bridge", "bridge", "Bridge edge loops", "Joins two loops of edges (or two faces) with a tunnel.", "&Mesh", false},
        {"smooth", "smooth", "Mark smooth", "Shades the selected edges (or the edges between selected faces) smooth.", "&Mesh", true},
        {"sharp", "sharp", "Clear smooth", "Makes the selected edges sharp again.", "&Mesh", false},
    };
    for (const ToolDef& t : tools) addTool(r, t);
    {
        ActionDef d;
        d.id = "mesh.subdivide";
        d.label = "Subdivide edges";
        d.category = "Mesh";
        d.description = "Splits each selected edge into cuts + 1 parts; edges of one face are joined across it.";
        d.menu = "&Mesh";
        d.contexts = {"mode:mesh"};
        d.params = json{{"cuts", 1}};
        d.variants = {{"S&ubdivide edges", {{"cuts", 1}}}, {"Subdivide edges x&2", {{"cuts", 2}}}, {"Subdivide edges x&3", {{"cuts", 3}}}};
        d.paletteKeyword = "subdivide";
        d.paletteParam = "cuts";
        d.paletteNumeric = true;
        d.enabled = inMeshMode;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->runMeshTool("subdivide", inv.params.is_object() ? inv.params.value("cuts", 1.0) : 1.0);
        };
        r.add(std::move(d));
    }
    const ToolDef selectTools[] = {
        {"select_all", "selectall", "Select all", "Selects every element of the brush.", "&Mesh/&Select", true},
        {"select_none", "selectnone", "Select none", "Clears the element selection.", "&Mesh/&Select", false},
        {"select_invert", "invert", "Invert selection", "Selects what is not selected.", "&Mesh/&Select", false},
        {"select_grow", "grow", "Grow selection", "Adds one ring of neighbours.", "&Mesh/&Select", false},
        {"select_shrink", "shrink", "Shrink selection", "Drops the elements that touch anything unselected.", "&Mesh/&Select", false},
    };
    for (const ToolDef& t : selectTools) addTool(r, t);
}

} // namespace native::ui
