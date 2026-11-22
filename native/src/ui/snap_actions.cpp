// ui/snap_actions -- see snap_actions.h.
#include "ui/snap_actions.h"

#include <cmath>
#include <functional>
#include <string>

#include "render/viewport_window.h"
#include "tools/status_text.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

using actions::ActionDef;
using actions::ActionEnv;
using actions::Invocation;
using json = nlohmann::json;

render::ViewportWindow* viewportOf(const ActionEnv& env) {
    const UiEnv* u = env.as<UiEnv>();
    return u ? u->viewport : nullptr;
}

bool haveViewport(const ActionEnv& env, const json&) { return viewportOf(env) != nullptr; }

void change(render::ViewportWindow* vp, const std::function<void(render::ViewportSettings&)>& fn) {
    render::ViewportSettings s = vp->rig().settings();
    const render::ViewportSettings before = s;
    fn(s);
    if (s == before) return;
    vp->rig().applySettings(s);
    vp->viewChanged();
}

// One snap value: variants for every listed value; `field` picks the setting.
void valueAction(actions::Registry& r, const char* id, const char* label, const char* menu, const char* description, const std::vector<double>& values, const char* unit,
                 double render::ViewportSettings::*field) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Snapping";
    d.description = description;
    d.kind = actions::ActionKind::Toggle;
    d.menu = menu;
    d.params = json{{"value", values.empty() ? 0.0 : values.front()}};
    for (const double v : values) d.variants.push_back({tools::shortNumber(v) + unit, json{{"value", v}}});
    d.enabled = haveViewport;
    d.checked = [field](const ActionEnv& env, const json& p) {
        const auto* vp = viewportOf(env);
        return vp && p.is_object() && std::abs(vp->rig().settings().*field - p.value("value", -1.0)) < 1e-9;
    };
    d.run = [field](ActionEnv& env, const Invocation& inv) {
        auto* vp = viewportOf(env);
        const double v = inv.params.is_object() ? inv.params.value("value", 0.0) : 0.0;
        if (!vp || !(v > 0)) return;
        change(vp, [&](render::ViewportSettings& s) { s.*field = v; });
    };
    r.add(std::move(d));
}

} // namespace

void registerSnapActions(actions::Registry& r) {
    {
        ActionDef d;
        d.id = "snap.toggle";
        d.label = "Snap to grid";
        d.category = "Snapping";
        d.description = "Turns snapping of moves, rotations and scales to the grid steps on or off for the active viewport.";
        d.kind = actions::ActionKind::Toggle;
        d.menu = "&View/&Snapping";
        d.enabled = haveViewport;
        d.checked = [](const ActionEnv& env, const json&) {
            const auto* vp = viewportOf(env);
            return vp && vp->rig().settings().snapEnabled;
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* vp = viewportOf(env)) change(vp, [](render::ViewportSettings& s) { s.snapEnabled = !s.snapEnabled; });
        };
        r.add(std::move(d));
    }
    valueAction(r, "snap.grid_size", "Grid size", "&View/&Snapping/Grid &size", "Sets the grid size (metres) of the active viewport.", render::gridSizes(), " m", &render::ViewportSettings::gridSize);
    valueAction(r, "snap.rotation", "Rotation snap", "&View/&Snapping/&Rotation snap", "Sets the rotation step (degrees) of the active viewport.", render::rotSnaps(), "\xC2\xB0", &render::ViewportSettings::rotSnap);
    valueAction(r, "snap.scale", "Scale snap", "&View/&Snapping/S&cale snap", "Sets the scale step of the active viewport.", render::scaleSnaps(), "", &render::ViewportSettings::scaleSnap);
}

} // namespace native::ui
