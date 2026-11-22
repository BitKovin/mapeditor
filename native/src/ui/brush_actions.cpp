// ui/brush_actions -- see brush_actions.h.
#include "ui/brush_actions.h"

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

// Brushes are selected, and nothing that owns the pointer is running.
bool canOperate(const ActionEnv& env, const json&) {
    MainWindow* w = windowOf(env);
    return w && w->document() && !w->meshMode() && !w->transforming() && !w->knifing() && !w->clipperActive() && !w->selection().empty();
}

void addSimple(actions::Registry& r, const char* id, const char* label, const char* description, bool separator, std::function<void(MainWindow&)> run) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Brush";
    d.description = description;
    d.menu = "&Brush";
    d.menuSeparatorBefore = separator;
    d.contexts = {"area:viewport"};
    d.enabled = canOperate;
    d.run = [run](ActionEnv& env, const Invocation&) {
        if (auto* w = windowOf(env)) run(*w);
    };
    r.add(std::move(d));
}

void addClipperKey(actions::Registry& r, const char* id, const char* label, const char* description, tools::Keep keep) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Brush";
    d.description = description;
    d.kind = ActionKind::Modal;
    d.contexts = {"brush:clipper"};
    d.enabled = [](const ActionEnv& env, const json&) {
        MainWindow* w = windowOf(env);
        return w && w->clipperActive();
    };
    d.run = [keep](ActionEnv& env, const Invocation&) {
        if (auto* w = windowOf(env)) w->clipperApply(keep);
    };
    r.add(std::move(d));
}

} // namespace

void registerBrushActions(actions::Registry& r) {
    {
        ActionDef d;
        d.id = "brush.clipper";
        d.label = "Clipper";
        d.category = "Brush";
        d.description = "Cuts the selected brushes along a plane given by two or three clicked points: Enter keeps the back side, Shift+Enter splits, Ctrl+Enter keeps the front.";
        d.kind = ActionKind::Toggle;
        d.menu = "&Brush";
        d.contexts = {"area:viewport"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->document() && !w->meshMode() && !w->transforming() && !w->knifing() && (w->clipperActive() || !w->selection().empty());
        };
        d.checked = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->clipperActive();
        };
        d.hint = "clipper";
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) {
                if (w->clipperActive()) w->stopClipper(QStringLiteral("Clipper off"));
                else w->startClipper();
            }
        };
        r.add(std::move(d));
    }
    addClipperKey(r, "brush.clip_back", "Clipper: keep back", "Cuts away the side the arrow points to.", tools::Keep::Back);
    addClipperKey(r, "brush.clip_both", "Clipper: split", "Splits each brush into two along the plane.", tools::Keep::Both);
    addClipperKey(r, "brush.clip_front", "Clipper: keep front", "Cuts away the side the arrow points away from.", tools::Keep::Front);
    {
        ActionDef d;
        d.id = "brush.clip_cancel";
        d.label = "Clipper: cancel";
        d.category = "Brush";
        d.description = "Drops the clipper points.";
        d.kind = ActionKind::Modal;
        d.contexts = {"brush:clipper"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->clipperActive();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->stopClipper(QStringLiteral("Clipper cancelled"));
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "brush.hollow";
        d.label = "Make hollow";
        d.category = "Brush";
        d.description = "Turns each selected convex brush into wall brushes of the given thickness (metres).";
        d.menu = "&Brush/&Hollow";
        d.contexts = {"area:viewport"};
        d.params = json{{"thickness", 0.25}};
        d.variants = {{"&0.125 m", {{"thickness", 0.125}}}, {"&0.25 m", {{"thickness", 0.25}}}, {"0.&5 m", {{"thickness", 0.5}}}, {"&1 m", {{"thickness", 1.0}}}};
        d.paletteKeyword = "hollow";
        d.paletteParam = "thickness";
        d.paletteNumeric = true;
        d.enabled = canOperate;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->hollowSelected(inv.params.is_object() ? inv.params.value("thickness", 0.25) : 0.25);
        };
        r.add(std::move(d));
    }
    addSimple(r, "brush.merge", "Merge brushes", "Makes the selected brushes one convex brush, when together they fill a convex shape.", false, [](MainWindow& w) { w.mergeSelected(); });
    addSimple(r, "brush.snap", "Snap to grid", "Moves the vertices of the selected brushes onto the grid (brushes that would break are left alone).", false, [](MainWindow& w) { w.snapSelected(); });
    for (const char* kind : {"flip", "rotate90"}) {
        const bool flip = std::string(kind) == "flip";
        ActionDef d;
        d.id = flip ? "brush.flip" : "brush.rotate90";
        d.label = flip ? "Flip" : "Rotate 90°";
        d.category = "Brush";
        d.description = flip ? "Mirrors the selection about its centre along the axis (Y is up)." : "Turns the selection by 90 degrees about an axis through its centre (dir -1 turns the other way).";
        d.menu = flip ? "&Brush/&Flip" : "&Brush/&Rotate 90°";
        d.menuSeparatorBefore = flip;
        d.contexts = {"area:viewport"};
        d.params = flip ? json{{"axis", "x"}} : json{{"axis", "x"}, {"dir", 1}};
        if (flip) d.variants = {{"&X", {{"axis", "x"}}}, {"&Y (up)", {{"axis", "y"}}}, {"&Z", {{"axis", "z"}}}};
        else d.variants = {{"&X", {{"axis", "x"}, {"dir", 1}}}, {"&Y (up)", {{"axis", "y"}, {"dir", 1}}}, {"&Z", {{"axis", "z"}, {"dir", 1}}},
                           {"X (the other way)", {{"axis", "x"}, {"dir", -1}}}, {"Y (the other way)", {{"axis", "y"}, {"dir", -1}}}, {"Z (the other way)", {{"axis", "z"}, {"dir", -1}}}};
        d.enabled = canOperate;
        d.run = [flip](ActionEnv& env, const Invocation& inv) {
            MainWindow* w = windowOf(env);
            if (!w) return;
            const std::string axis = inv.params.is_object() ? inv.params.value("axis", std::string("x")) : "x";
            const int a = axis == "y" ? 1 : axis == "z" ? 2 : 0;
            if (flip) w->flipSelected(a);
            else w->rotateSelected90(a, inv.params.is_object() ? inv.params.value("dir", 1) : 1);
        };
        r.add(std::move(d));
    }
    addSimple(r, "brush.select_inside", "Select inside", "Selects the other brushes that lie completely inside the selected brushes.", true, [](MainWindow& w) { w.selectByVolume(tools::VolumeMode::Inside); });
    addSimple(r, "brush.select_touching", "Select touching", "Selects the other brushes that overlap or touch the selected brushes.", false, [](MainWindow& w) { w.selectByVolume(tools::VolumeMode::Touching); });
}

} // namespace native::ui
