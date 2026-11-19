// ui/app_actions -- see app_actions.h.
#include "ui/app_actions.h"

#include <DockWidget.h>

#include <algorithm>

#include "render/viewport_window.h"
#include "ui/main_window.h"

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
    d.contexts = {"area:viewport"};
    d.params = json{{"op", "replace"}};
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
        d.id = "view.stats.toggle";
        d.label = "Stats";
        d.category = "View";
        d.kind = ActionKind::Toggle;
        d.menu = "&View";
        d.menuSeparatorBefore = true;
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
}

} // namespace native::ui
