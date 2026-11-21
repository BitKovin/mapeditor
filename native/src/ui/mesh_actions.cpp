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
    {
        struct Modal {
            const char* id;
            const char* label;
            const char* description;
            tools::ModalKind kind;
        };
        const Modal modals[] = {
            {"mesh.extrude", "Extrude", "Pushes the picked faces out along their normal with the pointer (typed digits give metres); one undo step.", tools::ModalKind::Extrude},
            {"mesh.inset", "Inset", "Insets each picked face by the pointer distance (typed digits give metres).", tools::ModalKind::Inset},
            {"mesh.bevel", "Bevel", "Chamfers the picked edges by the pointer distance; the mouse wheel adds or removes segments.", tools::ModalKind::Bevel},
        };
        for (const Modal& m : modals) {
            ActionDef d;
            d.id = m.id;
            d.label = m.label;
            d.category = "Mesh";
            d.description = m.description;
            d.menu = "&Mesh";
            d.menuSeparatorBefore = m.kind == tools::ModalKind::Extrude;
            d.contexts = {"mode:mesh"};
            d.enabled = [](const ActionEnv& env, const json&) {
                const UiEnv* u = env.as<UiEnv>();
                return u && u->window && u->viewport && u->window->meshMode() && !u->window->transforming();
            };
            const tools::ModalKind kind = m.kind;
            d.run = [kind](ActionEnv& env, const Invocation&) {
                const UiEnv* u = env.as<UiEnv>();
                if (u && u->window && u->viewport) u->window->startTransform(u->viewport, kind);
            };
            r.add(std::move(d));
        }
    }
    {
        ActionDef d;
        d.id = "mesh.loopcut";
        d.label = "Loop cut";
        d.category = "Mesh";
        d.description = "Cuts a loop across the faces around the edge under the pointer; move the pointer along the edge to place it (typed digits give 0..1), click to confirm.";
        d.menu = "&Mesh";
        d.contexts = {"mode:mesh"};
        d.enabled = [](const ActionEnv& env, const json&) {
            const UiEnv* u = env.as<UiEnv>();
            return u && u->window && u->viewport && u->window->meshMode() && !u->window->transforming();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            const UiEnv* u = env.as<UiEnv>();
            if (u && u->window && u->viewport) u->window->startLoopCut(u->viewport);
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.knife";
        d.label = "Knife";
        d.category = "Mesh";
        d.description = "Cuts the brush along clicked points on its vertices and edges (Ctrl = the middle of an edge); Enter or a right click cuts, Esc cancels.";
        d.menu = "&Mesh";
        d.contexts = {"mode:mesh"};
        d.enabled = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->meshMode() && !w->transforming() && !w->knifing();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->startKnife();
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.knife_finish";
        d.label = "Knife: cut";
        d.category = "Mesh";
        d.description = "Makes the cut along the points clicked so far.";
        d.kind = ActionKind::Modal;
        d.contexts = {"mesh:knife"};
        d.hint = "cut";
        d.enabled = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->knifing();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->finishKnife();
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.knife_cancel";
        d.label = "Knife: cancel";
        d.category = "Mesh";
        d.description = "Drops the points clicked so far.";
        d.kind = ActionKind::Modal;
        d.contexts = {"mesh:knife"};
        d.hint = "cancel";
        d.enabled = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->knifing();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->cancelKnife();
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.proportional";
        d.label = "Proportional editing";
        d.category = "Mesh";
        d.description = "Vertices near the picked ones follow G / R / S with a smooth falloff; the mouse wheel changes the radius while the tool runs.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Mesh";
        d.contexts = {"mode:mesh"};
        d.enabled = inMeshMode;
        d.checked = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->meshProportional();
        };
        d.hint = "proportional";
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->setMeshProportional(!w->meshProportional());
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "mesh.mirror";
        d.label = "Mirror";
        d.category = "Mesh";
        d.description = "Mirrors the picked vertices about their centre (all of them: the whole brush, turned inside out and back); X, Y or Z is the plane normal.";
        d.menu = "&Mesh/&Mirror";
        d.contexts = {"mode:mesh"};
        d.params = json{{"axis", 0}};
        d.variants = {{"&X", {{"axis", 0}}}, {"&Y", {{"axis", 1}}}, {"&Z", {{"axis", 2}}}};
        d.enabled = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->meshMode() && !w->transforming();
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->mirrorMesh(inv.params.is_object() ? inv.params.value("axis", 0) : 0);
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "transform.wheel";
        d.label = "Transform wheel";
        d.category = "Transform";
        d.description = "The mouse wheel during a mesh tool: the proportional radius of G / R / S, the segment count of a bevel.";
        d.kind = ActionKind::Axis;
        d.contexts = {"modal:transform"};
        d.enabled = [](const ActionEnv& env, const json&) {
            const MainWindow* w = windowOf(env);
            return w && w->transformUsesWheel();
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->transformWheel(inv.event.wheel);
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
