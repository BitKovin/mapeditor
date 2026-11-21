// ui/app_actions -- see app_actions.h.
#include "ui/app_actions.h"

#include <DockWidget.h>

#include <algorithm>

#include "render/viewport_window.h"
#include "ui/main_window.h"
#include "ui/brush_actions.h"
#include "ui/texture_actions.h"
#include "ui/mesh_actions.h"

namespace native::ui {

namespace {

using actions::ActionDef;
using actions::ActionEnv;
using actions::ActionKind;
using actions::InputEvent;
using actions::Invocation;
using json = nlohmann::json;

bool isRelease(const InputEvent& e) {
    return e.type == InputEvent::Type::KeyRelease || e.type == InputEvent::Type::MouseRelease;
}

render::ViewportWindow* viewportOf(const ActionEnv& env) {
    const UiEnv* u = env.as<UiEnv>();
    return u ? u->viewport : nullptr;
}

bool haveViewport(const ActionEnv& env, const json&) { return viewportOf(env) != nullptr; }

render::CameraMode modeFromName(const std::string& name) {
    if (name == "orbit") return render::CameraMode::Orbit;
    if (name == "top") return render::CameraMode::Top;
    if (name == "front") return render::CameraMode::Front;
    if (name == "side") return render::CameraMode::Side;
    return render::CameraMode::Fly;
}

void add(actions::Registry& r, ActionDef d) { r.add(std::move(d)); }

// A drag of the active viewport's camera: `pan` = middle-button style panning, else look / orbit / ortho pan.
void dragAction(actions::Registry& r, const char* id, const char* label, const char* context, bool pan, const char* holdContext = "") {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Camera";
    d.kind = ActionKind::Hold;
    d.contexts = {context};
    d.holdContext = holdContext;
    d.enabled = haveViewport;
    d.run = [pan](ActionEnv& env, const Invocation& inv) {
        render::ViewportWindow* vp = viewportOf(env);
        if (!vp) return;
        if (isRelease(inv.event)) {
            vp->stopFlight();   // the fly keys belong to the camera only while the look button is held
            return;
        }
        if (inv.event.type != InputEvent::Type::MouseMove) return;
        const double h = std::max(1, vp->height());
        if (pan) vp->rig().pan(inv.event.dx, inv.event.dy, h);
        else vp->rig().drag(inv.event.dx, inv.event.dy, h);
        vp->viewChanged();
    };
    add(r, std::move(d));
}

// Click = pick under the cursor, drag = box select (T3.5). `op` is the binding's parameter: replace (plain), toggle (Ctrl), face (Shift).
void selectAction(actions::Registry& r) {
    ActionDef d;
    d.id = "select.pick";
    d.label = "Select (click / box)";
    d.category = "Selection";
    d.description = "Click selects what is under the cursor, Ctrl+click toggles it, Shift+click selects the face; dragging a box selects what lies inside it.";
    d.kind = ActionKind::Hold;
    d.contexts = {"tool:select"};
    d.params = json{{"op", "replace"}};
    d.hint = "select";
    d.enabled = haveViewport;
    d.run = [](ActionEnv& env, const Invocation& inv) {
        const UiEnv* u = env.as<UiEnv>();
        if (!u || !u->window || !u->viewport) return;
        const std::string op = inv.params.is_object() ? inv.params.value("op", std::string("replace")) : std::string("replace");
        const MainWindow::PickOp pick = op == "toggle" ? MainWindow::PickOp::Toggle : op == "face" ? MainWindow::PickOp::Face : MainWindow::PickOp::Replace;
        u->window->pickInput(u->viewport, inv.event, pick);
    };
    add(r, std::move(d));
}

// Grid size of the active viewport: an exact size (grid.set {size}) or one step along gridSizes() (grid.smaller / grid.larger).
void gridActions(actions::Registry& r) {
    const auto apply = [](render::ViewportWindow* vp, double size) {
        render::ViewportSettings s = vp->rig().settings();
        if (s.gridSize == size) return;
        s.gridSize = size;
        vp->rig().applySettings(s);
        vp->viewChanged();
    };
    {
        ActionDef d;
        d.id = "grid.set";
        d.paletteKeyword = "grid";
        d.paletteParam = "size";
        d.paletteNumeric = true;
        d.label = "Set grid size";
        d.category = "Grid";
        d.description = "Sets the grid size (metres) of the active viewport; the size is the binding's parameter.";
        d.params = json{{"size", 0.5}};
        d.enabled = [](const ActionEnv& env, const json& p) {
            return viewportOf(env) && p.is_object() && p.value("size", 0.0) > 0;
        };
        d.run = [apply](ActionEnv& env, const Invocation& inv) {
            if (auto* vp = viewportOf(env)) apply(vp, inv.params.value("size", vp->rig().settings().gridSize));
        };
        add(r, std::move(d));
    }
    const auto step = [apply](int dir) {
        return [apply, dir](ActionEnv& env, const Invocation&) {
            auto* vp = viewportOf(env);
            if (!vp) return;
            const std::vector<double>& sizes = render::gridSizes();
            const double cur = vp->rig().settings().gridSize;
            // the nearest listed size at or below / above the current one
            double next = cur;
            if (dir < 0) {
                for (double g : sizes)
                    if (g < cur - 1e-9) next = g;
            } else {
                for (auto it = sizes.rbegin(); it != sizes.rend(); ++it)
                    if (*it > cur + 1e-9) next = *it;
            }
            apply(vp, next);
        };
    };
    for (const int dir : {-1, 1}) {
        ActionDef d;
        d.id = dir < 0 ? "grid.smaller" : "grid.larger";
        d.label = dir < 0 ? "Smaller grid" : "Larger grid";
        d.category = "Grid";
        d.description = "Steps the grid size of the active viewport along the list of grid sizes.";
        d.enabled = haveViewport;
        d.run = step(dir);
        add(r, std::move(d));
    }
}

void flyKeyAction(actions::Registry& r, const char* id, const char* label, render::FlyKey key) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Camera";
    d.description = "Moves the fly camera while the key is held and the look button is down (Shift = x3).";
    d.kind = ActionKind::Hold;
    d.contexts = {"hold:camera.fly"};
    d.enabled = haveViewport;
    d.run = [key](ActionEnv& env, const Invocation& inv) {
        render::ViewportWindow* vp = viewportOf(env);
        if (!vp) return;
        if (isRelease(inv.event)) {
            vp->flyKey(key, false);
            return;
        }
        vp->setBoost((inv.event.mods & actions::kShift) != 0);
        vp->flyKey(key, true);
    };
    add(r, std::move(d));
}

// The transform tools (T4.1): G / R / S start a modal operator in the viewport under the pointer; confirm, cancel and the axis keys act while it runs
// (context modal:transform); the gizmo mode of the active viewport; Esc leaves the origin editing.
void transformActions(actions::Registry& r) {
    const auto windowOf = [](const ActionEnv& env) { auto* u = env.as<UiEnv>(); return u ? u->window : nullptr; };
    struct Start {
        const char* id;
        const char* label;
        const char* description;
        tools::ModalKind kind;
    };
    const Start starts[] = {{"transform.grab", "Move (grab)", "Moves the selection with the pointer; X / Y / Z constrain, typed digits give an exact distance.", tools::ModalKind::Grab},
                            {"transform.rotate", "Rotate", "Rotates the selection about its pivot; X / Y / Z pick the axis, typed digits give degrees.", tools::ModalKind::Rotate},
                            {"transform.scale", "Scale", "Scales the selection about its pivot; X / Y / Z limit it to an axis, typed digits give the factor.", tools::ModalKind::Scale}};
    for (const Start& st : starts) {
        ActionDef d;
        d.id = st.id;
        d.label = st.label;
        d.category = "Transform";
        d.description = st.description;
        d.menu = "&Edit/&Transform";
        d.contexts = {"area:viewport"};
        const tools::ModalKind kind = st.kind;
        d.enabled = [windowOf](const ActionEnv& env, const json&) {
            auto* w = windowOf(env);
            return w && viewportOf(env) && w->canStartTransform();
        };
        d.run = [windowOf, kind](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->startTransform(viewportOf(env), kind);
        };
        add(r, std::move(d));
    }
    const auto running = [windowOf](const ActionEnv& env, const json&) {
        auto* w = windowOf(env);
        return w && w->transforming();
    };
    {
        ActionDef d;
        d.id = "transform.confirm";
        d.hint = "confirm";
        d.label = "Confirm transform";
        d.category = "Transform";
        d.kind = ActionKind::Modal;
        d.contexts = {"modal:transform"};
        d.enabled = running;
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->confirmTransform(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "transform.cancel";
        d.hint = "cancel";
        d.label = "Cancel transform";
        d.category = "Transform";
        d.kind = ActionKind::Modal;
        d.contexts = {"modal:transform"};
        d.enabled = running;
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->cancelTransform(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "transform.axis";
        d.hint = "constrain";
        d.label = "Constrain to axis";
        d.category = "Transform";
        d.description = "Limits the running transform to the world axis in the binding's parameter (0 = X, 1 = Y, 2 = Z); again lifts the limit.";
        d.kind = ActionKind::Modal;
        d.contexts = {"modal:transform"};
        d.params = json{{"axis", 0}};
        d.enabled = running;
        d.run = [windowOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->toggleTransformAxis(inv.params.value("axis", 0));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "gizmo.mode";
        d.label = "Gizmo";
        d.category = "Transform";
        d.description = "The transform gizmo of the active viewport: move arrows, rotation rings or scale handles.";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&Gizmo (active viewport)";
        d.params = json{{"mode", "move"}};
        d.variants = {{"&Move", {{"mode", "move"}}}, {"&Rotate", {{"mode", "rotate"}}}, {"&Scale", {{"mode", "scale"}}}};
        d.enabled = haveViewport;
        const auto modeOf = [](const json& p) {
            const std::string m = p.value("mode", std::string("move"));
            return m == "rotate" ? render::GizmoMode::Rotate : (m == "scale" ? render::GizmoMode::Scale : render::GizmoMode::Move);
        };
        d.checked = [modeOf](const ActionEnv& env, const json& p) {
            const auto* vp = viewportOf(env);
            return vp && vp->rig().settings().gizmo == modeOf(p);
        };
        d.run = [modeOf](ActionEnv& env, const Invocation& inv) {
            auto* vp = viewportOf(env);
            if (!vp) return;
            render::ViewportSettings s = vp->rig().settings();
            s.gizmo = modeOf(inv.params);
            vp->rig().applySettings(s);
            vp->viewChanged();
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "transform.leave_origin";
        d.label = "Leave origin editing";
        d.category = "Transform";
        d.contexts = {"area:viewport"};
        d.enabled = [windowOf](const ActionEnv& env, const json&) {
            auto* w = windowOf(env);
            return w && !w->originSelected().empty();
        };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->leaveOrigin(); };
        add(r, std::move(d));
    }
}

// The box / shape tool (T4.2): the tool and the shape of the block tools, the drawing itself (a left drag, then the height), cancelling.
void boxActions(actions::Registry& r) {
    const auto windowOf = [](const ActionEnv& env) { auto* u = env.as<UiEnv>(); return u ? u->window : nullptr; };
    {
        ActionDef d;
        d.id = "tool.set";
        d.label = "Tool";
        d.category = "Tools";
        d.description = "Switches between the select tool and the box tool.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Tools";
        d.params = json{{"tool", "select"}};
        d.variants = {{"&Select", {{"tool", "select"}}}, {"&Box", {{"tool", "box"}}}};
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        const auto toolOf = [](const json& p) { return p.value("tool", std::string("select")) == "box" ? MainWindow::Tool::Box : MainWindow::Tool::Select; };
        d.checked = [windowOf, toolOf](const ActionEnv& env, const json& p) {
            auto* w = windowOf(env);
            return w && w->tool() == toolOf(p);
        };
        d.run = [windowOf, toolOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->setTool(toolOf(inv.params));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "shape.set";
        d.label = "Shape";
        d.category = "Tools";
        d.description = "The solid the box tool draws: a box, cylinder, wedge, arch, stairs or sphere (selecting one switches to the box tool).";
        d.kind = ActionKind::Toggle;
        d.menu = "&Tools/S&hape";
        d.params = json{{"shape", "box"}};
        d.variants = {{"&Box", {{"shape", "box"}}},     {"&Cylinder", {{"shape", "cylinder"}}}, {"&Wedge", {{"shape", "wedge"}}},
                      {"&Arch", {{"shape", "arch"}}},   {"S&tairs", {{"shape", "stairs"}}},     {"&Sphere", {{"shape", "sphere"}}}};
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        const auto shapeOf = [](const json& p) {
            const std::string s = p.value("shape", std::string("box"));
            for (const auto id : {tools::ShapeId::Cylinder, tools::ShapeId::Wedge, tools::ShapeId::Arch, tools::ShapeId::Stairs, tools::ShapeId::Sphere})
                if (s == tools::shapeName(id)) return id;
            return tools::ShapeId::Box;
        };
        d.checked = [windowOf, shapeOf](const ActionEnv& env, const json& p) {
            auto* w = windowOf(env);
            return w && w->shape() == shapeOf(p);
        };
        d.run = [windowOf, shapeOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->setShape(shapeOf(inv.params));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "shape.turn";
        d.hint = "turn shape";
        d.label = "Turn shape";
        d.category = "Tools";
        d.description = "Turns the shape being drawn a quarter turn about its height: which way a ramp rises, stairs climb, an arch spans.";
        d.menu = "&Tools";
        d.contexts = {"tool:box"};
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->turnShape(1); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "box.draw";
        d.hint = "draw";
        d.label = "Draw box";
        d.category = "Tools";
        d.description = "Drag a rectangle on the surface under the pointer, move the pointer for the height and click to finish.";
        d.kind = ActionKind::Hold;
        d.contexts = {"tool:box"};
        d.enabled = haveViewport;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            const UiEnv* u = env.as<UiEnv>();
            if (u && u->window && u->viewport) u->window->boxInput(u->viewport, inv.event);
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "box.cancel";
        d.hint = "cancel";
        d.label = "Cancel drawing";
        d.category = "Tools";
        d.contexts = {"tool:box"};
        d.enabled = [windowOf](const ActionEnv& env, const json&) {
            auto* w = windowOf(env);
            return w && w->drawingBox();
        };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->cancelBox(); };
        add(r, std::move(d));
    }
}


// Delete, duplicate, copy / cut / paste, group, hide (T4.3): edits of the selection, each one undo step. Duplicate and paste go on into a grab so the copies can be placed.
void editActions(actions::Registry& r) {
    const auto windowOf = [](const ActionEnv& env) { auto* u = env.as<UiEnv>(); return u ? u->window : nullptr; };
    const auto hasSelection = [windowOf](const ActionEnv& env, const json&) {
        auto* w = windowOf(env);
        return w && w->document() && !w->selection().empty();
    };
    const auto hasDocument = [windowOf](const ActionEnv& env, const json&) {
        auto* w = windowOf(env);
        return w && w->document();
    };
    // grab after the copies exist: in the viewport under the pointer, else the active one
    const auto thenGrab = [windowOf](ActionEnv& env) {
        auto* w = windowOf(env);
        if (!w) return;
        render::ViewportWindow* vp = viewportOf(env) ? viewportOf(env) : w->activeViewport();
        if (vp) w->startTransform(vp, tools::ModalKind::Grab);
    };
    struct Def {
        const char* id;
        const char* label;
        const char* description;
        const char* menu;
        bool separator;
        int op;
    };
    const Def defs[] = {
        {"edit.delete", "Delete", "Deletes the selected brushes, entities and prefab instances.", "&Edit", true, 0},
        {"edit.duplicate", "Duplicate", "Copies the selection and moves the copies with the pointer.", "&Edit", false, 1},
        {"edit.copy", "Copy", "Copies the selection to the clipboard (the Shell can paste it).", "&Edit", true, 2},
        {"edit.cut", "Cut", "Copies the selection to the clipboard and deletes it.", "&Edit", false, 3},
        {"edit.group", "Group", "Moves the selected brushes into a new func_group entity.", "&Edit", true, 5},
        {"edit.ungroup", "Ungroup", "Moves the selected brushes (those of selected groups) back into the world.", "&Edit", false, 6},
        {"edit.hide", "Hide", "Hides the selection (a view setting: not saved, not undone).", "&Edit", true, 7},
    };
    for (const Def& df : defs) {
        ActionDef d;
        d.id = df.id;
        d.label = df.label;
        d.category = "Edit";
        d.description = df.description;
        d.menu = df.menu;
        d.menuSeparatorBefore = df.separator;
        d.contexts = {"area:viewport"};
        d.enabled = hasSelection;
        const int op = df.op;
        d.run = [windowOf, thenGrab, op](ActionEnv& env, const Invocation&) {
            auto* w = windowOf(env);
            if (!w) return;
            switch (op) {
                case 0: w->deleteSelected(); break;
                case 1: if (w->duplicateSelected()) thenGrab(env); break;
                case 2: w->copySelected(); break;
                case 3: w->cutSelected(); break;
                case 5: w->groupSelected(); break;
                case 6: w->ungroupSelected(); break;
                case 7: w->hideSelected(); break;
                default: break;
            }
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "edit.paste";
        d.label = "Paste";
        d.category = "Edit";
        d.description = "Pastes what was copied (also from the Shell) and moves the new items with the pointer.";
        d.menu = "&Edit";
        d.contexts = {"area:viewport"};
        d.enabled = hasDocument;
        d.run = [windowOf, thenGrab](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) if (w->pasteClipboard()) thenGrab(env);
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "edit.unhide_all";
        d.label = "Unhide all";
        d.category = "Edit";
        d.description = "Shows everything that was hidden with Hide or the Outliner eye.";
        d.menu = "&Edit";
        d.contexts = {"area:viewport"};
        d.enabled = [windowOf](const ActionEnv& env, const json&) {
            auto* w = windowOf(env);
            return w && !w->hideState().keys.empty();
        };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->unhideAll(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "view.hide_rule";
        d.label = "Hide class";
        d.category = "View";
        d.description = "Hides every entity whose class matches the pattern (trigger_* = every class starting with trigger_).";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&Hide";
        d.params = json{{"pattern", "trigger_*"}};
        d.variants = {{"&Triggers (trigger_*)", {{"pattern", "trigger_*"}}}, {"&Playable area (editor_playable_area)", {{"pattern", "editor_playable_area"}}}};
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        const auto enabledOf = [](const tools::HideState& h, const std::string& pattern) {
            const std::string p = tools::normalizePattern(pattern);
            for (const tools::HideRule& rule : h.rules)
                if (tools::normalizePattern(rule.pattern) == p) return rule.enabled;
            return false;
        };
        d.checked = [windowOf, enabledOf](const ActionEnv& env, const json& p) {
            auto* w = windowOf(env);
            return w && enabledOf(w->hideState(), p.value("pattern", std::string()));
        };
        d.run = [windowOf, enabledOf](ActionEnv& env, const Invocation& inv) {
            auto* w = windowOf(env);
            if (!w) return;
            const std::string pattern = inv.params.value("pattern", std::string());
            std::vector<tools::HideRule> rules = w->hideState().rules;
            if (enabledOf(w->hideState(), pattern)) {
                const std::string n = tools::normalizePattern(pattern);
                for (tools::HideRule& rule : rules)
                    if (tools::normalizePattern(rule.pattern) == n) rule.enabled = false;
            } else {
                rules = tools::addRule(rules, pattern);
            }
            w->setHideRules(rules);
        };
        add(r, std::move(d));
    }
}

// The command palette and the key help (T4.7): found from the Help menu, bound to F3 / Ctrl+Shift+P and the hold of F1 in the default keymap.
void helpActions(actions::Registry& r) {
    const auto windowOf = [](const ActionEnv& env) { auto* u = env.as<UiEnv>(); return u ? u->window : nullptr; };
    {
        ActionDef d;
        d.id = "view.command_palette";
        d.label = "Command palette...";
        d.category = "View";
        d.description = "Search every action by name and run it; recently used first. Type an argument after the name: grid 0.25.";
        d.menu = "&Help";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->openPalette(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "help.keys";
        d.label = "Key help (hold)";
        d.category = "Help";
        d.description = "While held, a sheet over the viewport lists the keys that work now.";
        d.kind = ActionKind::Hold;
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        d.run = [windowOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->showKeyHelp(!isRelease(inv.event));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "help.keys_toggle";
        d.label = "Key help";
        d.category = "Help";
        d.description = "Shows or hides the sheet over the viewport that lists the keys that work now.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Help";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        d.checked = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->keyHelpShown(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->showKeyHelp(!w->keyHelpShown()); };
        add(r, std::move(d));
    }
}

} // namespace

std::vector<std::string> viewportContexts(const render::ViewportWindow& viewport) {
    std::vector<std::string> c = {"area:viewport"};
    switch (viewport.rig().mode()) {
        case render::CameraMode::Fly: c.push_back("view:perspective"); c.push_back("view:fly"); break;
        case render::CameraMode::Orbit: c.push_back("view:perspective"); c.push_back("view:orbit"); break;
        default: c.push_back("view:ortho"); break;
    }
    return c;
}

void registerAppActions(actions::Registry& r) {
    // ---- file / edit ----
    const auto windowOf = [](const ActionEnv& env) { auto* u = env.as<UiEnv>(); return u ? u->window : nullptr; };
    {
        ActionDef d;
        d.id = "file.new";
        d.label = "New";
        d.category = "File";
        d.description = "Starts an empty map; Save then asks where to put it.";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->newDocument(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.open";
        d.label = "Open...";
        d.category = "File";
        d.description = "Opens a map from the assets folder.";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->openWithDialog(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.open_recent";
        d.label = "Open recent";
        d.category = "File";
        d.description = "Opens one of the maps opened or saved last.";
        d.menu = "&File/Open &Recent";
        d.params = {{"path", ""}};
        d.enabled = [windowOf](const ActionEnv& env, const json& p) {
            auto* w = windowOf(env);
            return w && !w->busy() && !p.value("path", std::string()).empty();
        };
        d.dynamicVariants = [windowOf](const ActionEnv& env) {
            std::vector<actions::MenuVariant> v;
            if (auto* w = windowOf(env)) {
                int n = 1;
                for (const std::string& path : w->recentFiles()) {
                    const size_t slash = path.find_last_of("/\\");
                    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
                    v.push_back({"&" + std::to_string(n++) + "  " + name, {{"path", path}}});
                }
            }
            return v;
        };
        d.run = [windowOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->openDocument(inv.params.value("path", std::string()));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.save";
        d.label = "Save";
        d.category = "File";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->document() && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->saveDocument(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.save_as";
        d.label = "Save As...";
        d.category = "File";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->document() && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->saveAsWithDialog(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.export";
        d.label = "Export...";
        d.category = "File";
        d.description = "Bakes the final lighting and writes the map in the game's format (game_profile.json \"exporter\"); runs on its own thread, progress in the status bar. While an export runs this cancels it.";
        d.menu = "&File";
        d.menuSeparatorBefore = true;
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->document() && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->exportWithDialog(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.cancel_export";
        d.label = "Cancel export";
        d.category = "File";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->exporting(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->cancelExport(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.play";
        d.label = "Play map";
        d.category = "File";
        d.description = "Exports the map next to its file and starts the game set in Settings > General > Run game with it.";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && w->document() && !w->busy() && !w->exporting(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->playMap(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.import_quake_map";
        d.label = "Import Quake map...";
        d.category = "File";
        d.description = "Converts a Quake family .map file (planes, Valve 220, brush primitives, patches) to a new map.";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->importQuakeMapWithDialog(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.set_assets_root";
        d.label = "Assets folder...";
        d.category = "File";
        d.description = "Chooses the folder maps are opened from and saved to.";
        d.menu = "&File";
        d.menuSeparatorBefore = true;
        d.enabled = [windowOf](const ActionEnv& env, const json&) { auto* w = windowOf(env); return w && !w->busy(); };
        d.run = [windowOf](ActionEnv& env, const Invocation&) { if (auto* w = windowOf(env)) w->chooseAssetsRootWithDialog(); };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.settings";
        d.label = "Settings...";
        d.category = "File";
        d.description = "The game folder, the game profile and the editor preferences.";
        d.menu = "&File";
        d.enabled = [windowOf](const ActionEnv& env, const json&) { return windowOf(env) != nullptr; };
        d.run = [windowOf](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->openSettings(inv.params.value("page", 0));
        };
        d.params = {{"page", 0}};
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "file.exit";
        d.label = "Exit";
        d.category = "File";
        d.menu = "&File";
        d.menuSeparatorBefore = true;
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* u = env.as<UiEnv>(); u && u->window) u->window->close();
        };
        add(r, std::move(d));
    }
    // Undo and redo (T2.4): the menu text names the step ("Undo Move").
    for (const bool undo : {true, false}) {
        ActionDef d;
        d.id = undo ? "edit.undo" : "edit.redo";
        d.label = undo ? "Undo" : "Redo";
        d.category = "Edit";
        d.description = undo ? "Reverts the last edit." : "Repeats the last undone edit.";
        d.menu = "&Edit";
        d.dynamicLabel = [windowOf, undo](const ActionEnv& env) {
            auto* w = windowOf(env);
            return w ? (undo ? w->undoText() : w->redoText()) : std::string();
        };
        d.enabled = [windowOf, undo](const ActionEnv& env, const json&) {
            auto* w = windowOf(env);
            return w && (undo ? w->canUndo() : w->canRedo());
        };
        d.run = [windowOf, undo](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) undo ? w->undo() : w->redo();
        };
        add(r, std::move(d));
    }

    // ---- view ----
    {
        ActionDef d;
        d.id = "view.layout";
        d.label = "Viewport layout";
        d.category = "View";
        d.description = "Shows 1, 2 or 4 viewports (perspective, top, front, side).";
        d.menu = "&View/&Layout";
        d.params = {{"count", 1}};
        d.variants = {{"&1 viewport", {{"count", 1}}}, {"&2 viewports", {{"count", 2}}}, {"&4 viewports", {{"count", 4}}}};
        d.enabled = [](const ActionEnv& env, const json&) { auto* u = env.as<UiEnv>(); return u && u->window && u->window->canLayout(); };
        d.checked = [](const ActionEnv& env, const json& p) {
            auto* u = env.as<UiEnv>();
            return u && u->window && static_cast<int>(u->window->layoutPreset()) == p.value("count", 1);
        };
        d.kind = ActionKind::Toggle;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            auto* u = env.as<UiEnv>();
            if (!u || !u->window) return;
            const int n = inv.params.value("count", 1);
            u->window->setLayoutPreset(n == 4 ? LayoutPreset::Four : (n == 2 ? LayoutPreset::Two : LayoutPreset::One));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "camera.mode";
        d.label = "Camera mode";
        d.category = "Camera";
        d.description = "Switches the active viewport between the perspective cameras (fly, orbit) and the orthographic views.";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&Camera (active viewport)";
        d.params = {{"mode", "fly"}};
        d.variants = {{"&Fly", {{"mode", "fly"}}},     {"&Orbit", {{"mode", "orbit"}}}, {"&Top", {{"mode", "top"}}},
                      {"F&ront", {{"mode", "front"}}}, {"&Side", {{"mode", "side"}}}};
        d.enabled = haveViewport;
        d.checked = [](const ActionEnv& env, const json& p) {
            const auto* vp = viewportOf(env);
            return vp && vp->rig().mode() == modeFromName(p.value("mode", "fly"));
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* vp = viewportOf(env)) vp->setCameraMode(modeFromName(inv.params.value("mode", "fly")));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "view.render_mode";
        d.label = "Render mode";
        d.category = "View";
        d.description = "Switches the active viewport between the unlit view and the lit views (Lit: textures times the live lighting; Lighting only: the light on white surfaces).";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&Render mode (active viewport)";
        d.params = {{"mode", "unlit"}};
        d.variants = {{"&Unlit", {{"mode", "unlit"}}}, {"&Lit", {{"mode", "lit"}}}, {"Lighting &only", {{"mode", "lightingOnly"}}}};
        d.enabled = [](const ActionEnv& env, const json& p) {
            const UiEnv* u = env.as<UiEnv>();
            if (!u || !u->viewport) return false;
            return p.value("mode", "unlit") == "unlit" || (u->window && u->window->litModesAvailable());
        };
        d.checked = [](const ActionEnv& env, const json& p) {
            const auto* vp = viewportOf(env);
            if (!vp) return false;
            const std::string m = p.value("mode", "unlit");
            const render::RenderMode cur = vp->rig().settings().renderMode;
            return m == "lit" ? cur == render::RenderMode::Lit : m == "lightingOnly" ? cur == render::RenderMode::LightingOnly : cur == render::RenderMode::Unlit;
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            auto* vp = viewportOf(env);
            if (!vp) return;
            const std::string m = inv.params.value("mode", "unlit");
            vp->setRenderMode(m == "lit" ? render::RenderMode::Lit : m == "lightingOnly" ? render::RenderMode::LightingOnly : render::RenderMode::Unlit);
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "view.dlss";
        d.label = "DLSS";
        d.category = "View";
        d.description = "NVIDIA DLSS Super Resolution for the active perspective viewport: Ultra Performance, Performance, Balanced and Quality render at 1/3, 1/2, 0.58 and 2/3 of the size and upscale, DLAA renders at full size and only removes the aliasing.";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&DLSS (active viewport)";
        d.params = {{"mode", "off"}};
        d.variants = {{"&Off", {{"mode", "off"}}}, {"&Ultra Performance", {{"mode", "ultraPerformance"}}}, {"&Performance", {{"mode", "performance"}}}, {"&Balanced", {{"mode", "balanced"}}},
                      {"&Quality", {{"mode", "quality"}}}, {"&DLAA", {{"mode", "dlaa"}}}};
        d.enabled = [](const ActionEnv& env, const json& p) {
            const UiEnv* u = env.as<UiEnv>();
            if (!u || !u->viewport) return false;
            return p.value("mode", "off") == "off" || (u->window && u->window->dlssAvailable());
        };
        d.checked = [](const ActionEnv& env, const json& p) {
            const auto* vp = viewportOf(env);
            if (!vp) return false;
            return vp->rig().settings().dlss == render::dlssModeFromName(p.value("mode", "off"));
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            auto* vp = viewportOf(env);
            if (!vp) return;
            vp->setDlssMode(render::dlssModeFromName(inv.params.value("mode", "off")));
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "view.grid";
        d.label = "Grid";
        d.category = "View";
        d.description = "Shows or hides the ground grid of the active viewport.";
        d.kind = ActionKind::Toggle;
        d.menu = "&View/&Grid (active viewport)";
        d.enabled = [](const ActionEnv& env, const json&) {
            const UiEnv* u = env.as<UiEnv>();
            return u && u->viewport;
        };
        d.checked = [](const ActionEnv& env, const json&) {
            const auto* vp = viewportOf(env);
            return vp && vp->rig().settings().showGrid;
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            auto* vp = viewportOf(env);
            if (vp) vp->setShowGrid(!vp->rig().settings().showGrid);
        };
        add(r, std::move(d));
    }
    {   // the dock panels: closing one with its x leaves it reachable here
        struct Panel {
            const char* id;
            const char* label;
            ads::CDockWidget* (MainWindow::*dock)() const;
        };
        static const Panel panels[] = {{"view.place.toggle", "Place", &MainWindow::placeDock},
                                       {"view.outliner.toggle", "Outliner", &MainWindow::outlinerDock},
                                       {"view.details.toggle", "Details", &MainWindow::detailsDock},
                                       {"view.surface.toggle", "Surface", &MainWindow::surfaceDock}};
        bool first = true;
        for (const Panel& p : panels) {
            ActionDef d;
            d.id = p.id;
            d.label = p.label;
            d.category = "View";
            d.kind = ActionKind::Toggle;
            d.menu = "&View";
            d.menuSeparatorBefore = first;
            first = false;
            const auto dockOf = [dock = p.dock](const ActionEnv& env) -> ads::CDockWidget* {
                auto* u = env.as<UiEnv>();
                return u && u->window ? (u->window->*dock)() : nullptr;
            };
            d.enabled = [dockOf](const ActionEnv& env, const json&) { return dockOf(env) != nullptr; };
            d.checked = [dockOf](const ActionEnv& env, const json&) {
                auto* w = dockOf(env);
                return w && !w->isClosed();
            };
            d.run = [dockOf](ActionEnv& env, const Invocation&) {
                if (auto* w = dockOf(env)) w->toggleView(w->isClosed());
            };
            add(r, std::move(d));
        }
    }
    {
        ActionDef d;
        d.id = "view.stats.toggle";
        d.label = "Stats";
        d.category = "View";
        d.kind = ActionKind::Toggle;
        d.menu = "&View";
        d.enabled = [](const ActionEnv& env, const json&) { auto* u = env.as<UiEnv>(); return u && u->window; };
        d.checked = [](const ActionEnv& env, const json&) {
            auto* u = env.as<UiEnv>();
            return u && u->window && !u->window->statsDock()->isClosed();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            auto* u = env.as<UiEnv>();
            if (u && u->window) u->window->statsDock()->toggleView(u->window->statsDock()->isClosed());
        };
        add(r, std::move(d));
    }

    // ---- cameras (the input of a viewport) ----
    dragAction(r, "camera.look", "Look (fly camera)", "view:fly", false, "hold:camera.fly");
    dragAction(r, "camera.orbit", "Orbit", "view:orbit", false);
    dragAction(r, "camera.pan2d", "Pan (orthographic view)", "view:ortho", false);
    dragAction(r, "camera.pan", "Pan", "area:viewport", true);
    selectAction(r);
    transformActions(r);
    boxActions(r);
    editActions(r);
    gridActions(r);
    {
        ActionDef d;
        d.id = "camera.dolly";
        d.label = "Dolly / zoom";
        d.category = "Camera";
        d.kind = ActionKind::Axis;
        d.contexts = {"area:viewport"};
        d.enabled = haveViewport;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* vp = viewportOf(env)) {
                vp->rig().wheel(inv.event.wheel, false);
                vp->viewChanged();
            }
        };
        add(r, std::move(d));
    }
    {
        ActionDef d;
        d.id = "camera.fly.speed";
        d.label = "Fly speed";
        d.category = "Camera";
        d.description = "Changes the fly speed by x1.2 per notch while flying.";
        d.kind = ActionKind::Axis;
        d.contexts = {"hold:camera.fly"};
        d.enabled = haveViewport;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* vp = viewportOf(env)) {
                vp->rig().wheel(inv.event.wheel, true);
                vp->viewChanged();
            }
        };
        add(r, std::move(d));
    }
    flyKeyAction(r, "camera.fly.forward", "Fly forward", render::FlyKey::Forward);
    flyKeyAction(r, "camera.fly.back", "Fly back", render::FlyKey::Back);
    flyKeyAction(r, "camera.fly.left", "Fly left", render::FlyKey::Left);
    flyKeyAction(r, "camera.fly.right", "Fly right", render::FlyKey::Right);
    flyKeyAction(r, "camera.fly.up", "Fly up", render::FlyKey::Up);
    flyKeyAction(r, "camera.fly.down", "Fly down", render::FlyKey::Down);
    {
        ActionDef d;
        d.id = "camera.fly.boost";
        d.label = "Fly boost";
        d.category = "Camera";
        d.description = "Flies 3x faster while held.";
        d.kind = ActionKind::Hold;
        d.contexts = {"hold:camera.fly"};
        d.enabled = haveViewport;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* vp = viewportOf(env)) vp->setBoost(!isRelease(inv.event));
        };
        add(r, std::move(d));
    }
    helpActions(r);
    registerMeshActions(r);
    registerBrushActions(r);
    registerTextureActions(r);
}

} // namespace native::ui
