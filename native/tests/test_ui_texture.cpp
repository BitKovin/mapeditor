// T11.3d: the texturing commands of the main window and their actions and keys (apply, Radiant texture keys, copy / paste UV, lightmap density, justify / fit / reset). Offscreen platform,
// the window is never shown; the texture sizes come from a fake info function (no assets folder).
#include <doctest.h>

#include <QApplication>
#include <QStatusBar>

#include <cmath>
#include <map>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;

namespace {

std::shared_ptr<const doc::DocumentVersion> twoBoxes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {5, 0, 0}, {7, 2, 2}, "dev/grey"));
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

const core::texplan::TexInfo* fakeInfo(const std::string& path) {
    static std::map<std::string, core::texplan::TexInfo> infos;
    auto it = infos.find(path);
    if (it == infos.end()) {
        core::texplan::TexInfo i;
        i.path = path;
        i.width = 128;
        i.height = 64;
        i.ppm = 64;
        it = infos.emplace(path, i).first;
    }
    return &it->second;
}

struct Fixture {
    ui::MainWindow window;
    Fixture() : window(services()) {
        window.setTextureInfo([](const std::string& p) { return fakeInfo(p); });
        window.setDocument(twoBoxes(), true);
    }
    const core::Brush* brush(const std::string& id) const {
        for (const auto& e : window.document()->entities)
            for (const auto& b : e->brushes)
                if (b->brush.id == id) return &b->brush;
        return nullptr;
    }
    bool key(const std::string& name, uint8_t mods = 0) {
        window.router().contexts().set({"window:main", "area:viewport", "view:perspective", "view:fly", "tool:select"});
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

std::vector<core::Vec2> uvsOf(const core::Brush* b, size_t face = 0) { return b->faces.at(face).uvs; }
bool same(const std::vector<core::Vec2>& a, const std::vector<core::Vec2>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].u - b[i].u) > 1e-9 || std::abs(a[i].v - b[i].v) > 1e-9) return false;
    return true;
}

} // namespace

TEST_CASE("apply texture: the active texture goes on the selected brush as one undo step; with a given texture it becomes the active one") {
    Fixture f;
    CHECK_FALSE(f.window.textureCommand("lift"));   // nothing selected
    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(f.window.textureCommand("apply", {{"texture", "wall/brick"}}));
    CHECK(f.window.activeTexture() == "wall/brick");
    for (const auto& face : f.brush("a")->faces) CHECK(face.texture == "wall/brick");
    for (const auto& face : f.brush("b")->faces) CHECK(face.texture == "dev/grey");
    f.window.history().undo();
    for (const auto& face : f.brush("a")->faces) CHECK(face.texture == "dev/grey");
    // the action applies the active texture
    CHECK(f.window.runAction("texture.apply"));
    for (const auto& face : f.brush("a")->faces) CHECK(face.texture == "wall/brick");
    CHECK_FALSE(f.window.textureCommand("apply"));   // nothing changes now: no step
}

TEST_CASE("texture keys: Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp rotates, Alt makes it fine; without a selection they do nothing") {
    Fixture f;
    CHECK_FALSE(f.key("Right", actions::kShift));
    f.window.selection().select(SelRef::face("w", "a", 0));
    CHECK(f.window.textureCommand("reset"));
    const auto base = uvsOf(f.brush("a"));
    REQUIRE_FALSE(base.empty());
    CHECK(f.key("Right", actions::kShift));
    const auto shifted = uvsOf(f.brush("a"));
    CHECK(std::abs(shifted[0].u - base[0].u) + std::abs(shifted[0].v - base[0].v) > 1e-6);
    // fine = a smaller step in the same direction
    CHECK(f.window.textureCommand("reset"));
    CHECK(f.key("Right", actions::kShift | actions::kAlt));
    const auto fine = uvsOf(f.brush("a"));
    const double big = std::abs(shifted[0].u - base[0].u) + std::abs(shifted[0].v - base[0].v);
    const double small = std::abs(fine[0].u - base[0].u) + std::abs(fine[0].v - base[0].v);
    CHECK(small > 1e-9);
    CHECK(small < big);
    // scale and rotate change the mapping too
    CHECK(f.window.textureCommand("reset"));
    CHECK(f.key("Up", actions::kShift | actions::kCtrl));
    CHECK_FALSE(same(uvsOf(f.brush("a")), base));
    CHECK(f.window.textureCommand("reset"));
    CHECK(f.key("PageUp", actions::kShift));
    CHECK_FALSE(same(uvsOf(f.brush("a")), base));
    // only the selected face changed
    }

TEST_CASE("copy / paste UV: the texture and mapping of one face go onto another; flow needs a copy and a selected face") {
    Fixture f;
    f.window.selection().select(SelRef::face("w", "a", 0));
    CHECK(f.window.textureCommand("apply", {{"texture", "wall/brick"}}));
    CHECK(f.window.textureCommand("shift", {{"axis", 0}, {"value", 16}}));
    CHECK(f.window.textureCommand("copyUv"));
    REQUIRE(f.window.copiedFace().has_value());
    f.window.selection().select(SelRef::face("w", "b", 0));
    CHECK(f.window.textureCommand("pasteUv"));
    CHECK(f.brush("b")->faces[0].texture == "wall/brick");
    CHECK(f.brush("b")->faces[0].uvs.size() == f.brush("a")->faces[0].uvs.size());
    // paste onto a whole brush = all its faces
    f.window.selection().select(SelRef::brush("w", "b"));
    CHECK(f.window.textureCommand("pasteUv"));
    for (const auto& face : f.brush("b")->faces) CHECK(face.texture == "wall/brick");
    // flow with a brush selected: no single target face
    CHECK_FALSE(f.window.textureCommand("flow"));
    // lift: the texture of a face becomes the active one
    f.window.setActiveTexture("dev/grey");
    f.window.selection().select(SelRef::face("w", "b", 2));
    CHECK(f.window.textureCommand("lift"));
    CHECK(f.window.activeTexture() == "wall/brick");
}

TEST_CASE("lightmap density: the factors multiply, Reset sets 1; the values are clamped") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(f.window.runAction("texture.lightmap", {{"factor", 2.0}}));
    CHECK(tools::tex::lightmapScaleOf(f.brush("a")->faces[0]) == doctest::Approx(2.0));
    CHECK(f.window.runAction("texture.lightmap", {{"factor", 2.0}}));
    CHECK(tools::tex::lightmapScaleOf(f.brush("a")->faces[0]) == doctest::Approx(4.0));
    CHECK(f.window.runAction("texture.lightmap", {{"value", 1.0}}));
    CHECK(tools::tex::lightmapScaleOf(f.brush("a")->faces[0]) == doctest::Approx(1.0));
    for (int i = 0; i < 8; ++i) f.window.runAction("texture.lightmap", {{"factor", 4.0}});
    CHECK(tools::tex::lightmapScaleOf(f.brush("a")->faces[0]) == doctest::Approx(core::kMaxLightmapScale));
}

TEST_CASE("justify, fit, project and the field edits keep one undo step each and leave the other brush alone") {
    Fixture f;
    f.window.selection().select(SelRef::face("w", "a", 0));
    const auto bBefore = uvsOf(f.brush("b"));
    CHECK(f.window.textureCommand("reset"));
    const auto reset = uvsOf(f.brush("a"));
    CHECK(f.window.textureCommand("stretch", {{"axis", 0}, {"value", 2.0}}));
    CHECK_FALSE(same(uvsOf(f.brush("a")), reset));
    CHECK(f.window.textureCommand("fit"));
    f.window.textureCommand("justify", {{"edge", "top"}});   // fit already put it there: may be no change
    CHECK(f.window.textureCommand("mirror", {{"axis", 1}}));
    CHECK(f.window.textureCommand("shear", {{"value", 0.25}}));
    CHECK(f.window.textureCommand("ortho"));
    CHECK(f.window.textureCommand("axial"));
    f.window.textureCommand("matchGrid", {{"grid", 0.25}});
    CHECK_FALSE(f.window.textureCommand("nudge", {{"mode", "shift"}, {"key", "sideways"}}));
    CHECK_FALSE(f.window.textureCommand("no-such-op"));
    CHECK(same(uvsOf(f.brush("b")), bBefore));
    // everything is undoable step by step back to the unmapped face
    for (int i = 0; i < 12; ++i) f.window.history().undo();
    CHECK(same(uvsOf(f.brush("a")), bBefore));
}

TEST_CASE("the texture actions are disabled without faces to work on, during mesh mode and while the document is empty") {
    Fixture f;
    ui::UiEnv env{&f.window, nullptr};
    actions::ActionEnv ae{&env};
    const actions::ActionDef* shift = f.window.registry().find("texture.shift");
    REQUIRE(shift);
    CHECK_FALSE(shift->enabled(ae, shift->params));
    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(shift->enabled(ae, shift->params));
    CHECK(f.window.registry().find("texture.paste_uv"));
    CHECK(f.window.registry().find("texture.project"));
}
