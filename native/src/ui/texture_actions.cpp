// ui/texture_actions -- see texture_actions.h.
#include "ui/texture_actions.h"

#include <string>

#include "ui/app_actions.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

using actions::ActionDef;
using actions::ActionEnv;
using actions::Invocation;
using json = nlohmann::json;

MainWindow* windowOf(const ActionEnv& env) {
    const UiEnv* u = env.as<UiEnv>();
    return u ? u->window : nullptr;
}

// Texture edits have faces to work on, and nothing that owns the pointer is running.
bool canTexture(const ActionEnv& env, const json&) {
    MainWindow* w = windowOf(env);
    return w && w->document() && !w->meshMode() && !w->transforming() && !w->knifing() && !w->clipperActive() && w->textureTargetsExist();
}

// An action that runs the window's textureCommand `op` with its own params (or with `fixed` merged under them).
void add(actions::Registry& r, const char* id, const char* label, const char* description, const char* menu, bool separator, const char* op, const json& params = json::object(),
         bool needsTargets = true) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Texture";
    d.description = description;
    d.menu = menu;
    d.menuSeparatorBefore = separator;
    d.contexts = {"area:viewport"};
    if (needsTargets) d.enabled = canTexture;
    else d.enabled = [](const ActionEnv& env, const json&) {
        MainWindow* w = windowOf(env);
        return w && w->document() && !w->meshMode() && !w->transforming();
    };
    if (!params.empty()) d.params = params;
    const std::string o = op;
    d.run = [o](ActionEnv& env, const Invocation& inv) {
        if (auto* w = windowOf(env)) w->textureCommand(o, inv.params);
    };
    r.add(std::move(d));
}

// Shift / scale / rotate keys: params {key, fine}; the op is fixed by the action and the window gets `mode`.
void addKey(actions::Registry& r, const char* id, const char* label, const char* description, const char* mode) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Texture";
    d.description = description;
    d.contexts = {"area:viewport"};
    d.params = json{{"key", "left"}, {"fine", false}};
    d.enabled = canTexture;
    const std::string m = mode;
    d.run = [m](ActionEnv& env, const Invocation& inv) {
        if (auto* w = windowOf(env)) {
            json p = inv.params.is_object() ? inv.params : json::object();
            p["mode"] = m;
            w->textureCommand("nudge", p);
        }
    };
    r.add(std::move(d));
}

} // namespace

void registerTextureActions(actions::Registry& r) {
    add(r, "texture.apply", "Apply active texture", "Puts the active texture on the selected faces (or all faces of the selected brushes).", "&Texture", false, "apply", json::object(), false);
    add(r, "texture.lift", "Lift material", "Makes the texture of the selected face the active texture.", "&Texture", false, "lift", json::object(), false);
    add(r, "texture.copy_uv", "Copy UV", "Remembers the texture and mapping of the selected face.", "&Texture", true, "copyUv", json::object(), false);
    add(r, "texture.paste_uv", "Paste UV", "Puts the copied texture and mapping on the selected faces (all faces of selected brushes).", "&Texture", false, "pasteUv");
    add(r, "texture.flow", "Flow texture", "Continues the copied face's texture onto the selected face, across the shared edges.", "&Texture", false, "flow");
    add(r, "texture.hotspot", "Hotspot", "Shows on each selected face the hotspot of its texture that fits the face best.", "&Texture", true, "hotspot");
    add(r, "texture.reset", "Reset texture", "The texture at its natural size, unrotated, from the face's bottom-left corner.", "&Texture", false, "reset");
    add(r, "texture.fit", "Fit texture", "Stretches the texture so it covers each face exactly once.", "&Texture", false, "fit");
    add(r, "texture.match_grid", "Match grid", "Every repeat becomes a whole number of grid cells wide.", "&Texture", false, "matchGrid");
    {
        ActionDef d;
        d.id = "texture.justify";
        d.label = "Justify texture";
        d.category = "Texture";
        d.description = "Moves the texture so its edge touches the same edge of the face (or centres it).";
        d.menu = "&Texture/&Justify";
        d.contexts = {"area:viewport"};
        d.params = json{{"edge", "center"}};
        d.variants = {{"&Top", {{"edge", "top"}}}, {"&Bottom", {{"edge", "bottom"}}}, {"&Left", {{"edge", "left"}}}, {"&Right", {{"edge", "right"}}}, {"&Center", {{"edge", "center"}}}};
        d.enabled = canTexture;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->textureCommand("justify", inv.params);
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "texture.project";
        d.label = "Project texture";
        d.category = "Texture";
        d.description = "Projects the texture along the dominant axis (axial), in the face plane (ortho) or from the active viewport's camera (cam).";
        d.menu = "&Texture/&Project";
        d.contexts = {"area:viewport"};
        d.params = json{{"kind", "axial"}};
        d.variants = {{"&Axial", {{"kind", "axial"}}}, {"&Ortho", {{"kind", "ortho"}}}, {"&Cam", {{"kind", "cam"}}}};
        d.enabled = canTexture;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            auto* w = windowOf(env);
            if (!w) return;
            const std::string kind = inv.params.is_object() ? inv.params.value("kind", std::string("axial")) : "axial";
            w->textureCommand(kind == "cam" ? "cam" : kind == "ortho" ? "ortho" : "axial");
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "texture.lightmap";
        d.label = "Lightmap density";
        d.category = "Texture";
        d.description = "Multiplies the lightmap texel density of the selected faces (x2 = finer, 0.5 = coarser) or sets it.";
        d.menu = "&Texture/&Lightmap density";
        d.contexts = {"area:viewport"};
        d.params = json{{"factor", 2.0}};
        d.variants = {{"&Finer (x2)", {{"factor", 2.0}}}, {"&Coarser (/2)", {{"factor", 0.5}}}, {"&Reset (1)", {{"value", 1.0}}}};
        d.enabled = canTexture;
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->textureCommand("lightmap", inv.params);
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "texture.ftt";
        d.label = "Fast Texture Tool";
        d.category = "Texture";
        d.description = "Opens (or closes) the window that edits the UV island of the selected faces by direct manipulation.";
        d.menu = "&Texture";
        d.menuSeparatorBefore = true;
        d.contexts = {"area:viewport"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->document() && !w->meshMode() && !w->transforming();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->toggleFastTextureTool();
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "texture.properties";
        d.label = "Texture properties...";
        d.category = "Texture";
        d.description = "Opens the properties of the active texture: pixels per metre, repetition, transparency and hotspots.";
        d.menu = "&Texture";
        d.menuSeparatorBefore = true;
        d.contexts = {"area:viewport"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->document() && !w->activeTexture().empty();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->openTextureProperties(w->activeTexture());
        };
        r.add(std::move(d));
    }
    {
        ActionDef d;
        d.id = "texture.group.new";
        d.label = "New texture group...";
        d.category = "Texture";
        d.description = "Opens the texture group editor on a new group (starts from a template).";
        d.menu = "&Texture";
        d.contexts = {"area:viewport"};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->document();
        };
        d.run = [](ActionEnv& env, const Invocation&) {
            if (auto* w = windowOf(env)) w->openTexGroupEditor({});
        };
        r.add(std::move(d));
    }
    addKey(r, "texture.shift", "Shift texture", "Moves the texture on the selected faces by one step (keys: Shift+arrows; Alt = fine).", "shift");
    addKey(r, "texture.scale", "Scale texture", "Scales the texture on the selected faces by one step (keys: Ctrl+Shift+arrows; Alt = fine).", "scale");
    addKey(r, "texture.rotate", "Rotate texture", "Rotates the texture on the selected faces by one step (keys: Shift+PageUp / PageDown; Alt = fine).", "rotate");
}

} // namespace native::ui
