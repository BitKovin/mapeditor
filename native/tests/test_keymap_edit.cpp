// T4.6: the binding editor model (app/actions/keymap_edit): contexts, conflicts, rows and filters, reverse lookup, editing with undo, the keymap files,
// and the acceptance rules on the real registry and the shipped default preset. Headless.
#include <doctest.h>

#include <QFile>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "app/actions/keymap_edit.h"
#include "app/actions/router.h"
#include "ui/app_actions.h"
#include "ui/chord_capture.h"

using namespace native;
using namespace native::actions;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() / ("native_keymap_test_" + std::to_string(++n) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readAsset(const char* relative) {
    QFile f(QString::fromUtf8(NATIVE_ASSETS_DIR) + "/" + QString::fromUtf8(relative));
    REQUIRE_MESSAGE(f.open(QIODevice::ReadOnly), "cannot open ", f.fileName().toStdString());
    return f.readAll().toStdString();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void addAction(Registry& r, const std::string& id, const std::string& label, const std::string& category, std::vector<std::string> contexts = {}) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = category;
    d.contexts = std::move(contexts);
    r.add(std::move(d));
}

// Delete in the viewport, Dissolve on the same key in the box tool, Save on Ctrl+S everywhere.
struct Fixture {
    Registry registry;
    Keymap keymap;
    Fixture() {
        addAction(registry, "edit.delete", "Delete", "Edit", {"area:viewport"});
        addAction(registry, "mesh.dissolve", "Dissolve", "Mesh", {"tool:box"});
        addAction(registry, "file.save", "Save", "File");
        addAction(registry, "view.grid", "Grid", "View", {"area:viewport"});
        addAction(registry, "cam.fly", "Fly", "Camera", {"hold:camera.fly"});
        const char* preset = R"({"name": "t", "bindings": [
            {"action": "edit.delete", "context": "area:viewport", "input": "X"},
            {"action": "edit.delete", "context": "area:viewport", "input": "Delete"},
            {"action": "mesh.dissolve", "context": "tool:box", "input": "Ctrl+X"},
            {"action": "file.save", "context": "global", "input": "Ctrl+S"} ]})";
        std::string err;
        REQUIRE_MESSAGE(keymap.loadPreset(preset, &err), err);
    }
};

Binding bind(const std::string& action, const std::string& context, const std::string& input) {
    Binding b;
    std::string err;
    REQUIRE_MESSAGE(makeBinding(action, context, input, nullptr, false, b, &err), err);
    return b;
}

InputEvent key(const std::string& k, uint8_t mods = 0) {
    InputEvent e;
    e.type = InputEvent::Type::KeyPress;
    e.key = e.scan = k;
    e.mods = mods;
    return e;
}

} // namespace

TEST_CASE("contexts: levels, overlap, defaults") {
    CHECK(contextLevel("global") == 0);
    CHECK(contextLevel("window:main") == 1);
    CHECK(contextLevel("area:viewport") == 2);
    CHECK(contextLevel("view:ortho") == 3);
    CHECK(contextLevel("mode:mesh.edge") == 4);
    CHECK(contextLevel("tool:box") == 5);
    CHECK(contextLevel("modal:transform") == 6);
    CHECK(contextLevel("hold:camera.fly") == 6);

    CHECK(contextsOverlap("global", "tool:box"));
    CHECK(contextsOverlap("area:viewport", "tool:select"));
    CHECK(contextsOverlap("tool:box", "tool:box"));
    CHECK_FALSE(contextsOverlap("tool:box", "tool:select"));          // one tool at a time
    CHECK_FALSE(contextsOverlap("area:viewport", "area:outliner"));
    CHECK(contextsOverlap("view:perspective", "view:fly"));
    CHECK_FALSE(contextsOverlap("view:ortho", "view:orbit"));
    CHECK(contextsOverlap("modal:transform", "hold:camera.fly"));

    ActionDef d;
    CHECK(defaultContextFor(d) == "global");
    d.contexts = {"area:viewport", "tool:box"};
    CHECK(defaultContextFor(d) == "tool:box");

    Chord c;
    REQUIRE(parseChord("Ctrl+K", c));
    CHECK(bindableChord(c));
    REQUIRE(parseChord("Meta+K", c));
    std::string why;
    CHECK_FALSE(bindableChord(c, &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(bindableChord(Chord{}));
}

TEST_CASE("conflicts: same context, more specific and less specific contexts") {
    Fixture f;
    // X is Delete in the viewport: the same input in the same context
    auto cs = conflictsFor(f.registry, f.keymap, bind("view.grid", "area:viewport", "X"));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == Conflict::Kind::Same);
    CHECK(cs[0].actionLabel == "Delete");
    CHECK(describeConflict(cs[0]).find("Delete") != std::string::npos);
    // the same action with the same parameters is no clash
    CHECK(conflictsFor(f.registry, f.keymap, bind("edit.delete", "area:viewport", "X")).empty());
    // a more specific context takes the input over where both are active
    cs = conflictsFor(f.registry, f.keymap, bind("mesh.dissolve", "tool:box", "X"));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == Conflict::Kind::Shadows);
    // a less specific one would never run where the existing binding is active
    cs = conflictsFor(f.registry, f.keymap, bind("file.save", "global", "X"));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == Conflict::Kind::Shadowed);
    // contexts that are never active together do not clash
    CHECK(conflictsFor(f.registry, f.keymap, bind("mesh.dissolve", "tool:select", "Ctrl+X")).empty());
    // a free input
    CHECK(conflictsFor(f.registry, f.keymap, bind("view.grid", "area:viewport", "Ctrl+Alt+G")).empty());
}

TEST_CASE("rows: bindings, flags and filters") {
    Fixture f;
    auto rows = buildRows(f.registry, f.keymap);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].def->id == "edit.delete");
    CHECK(rows[0].bindings.size() == 2);
    CHECK_FALSE(rows[0].changed);
    CHECK(rows[3].unbound);   // view.grid

    RowFilter unbound;
    unbound.unbound = true;
    rows = buildRows(f.registry, f.keymap, unbound);
    REQUIRE(rows.size() == 2);   // view.grid, cam.fly
    CHECK(rows[0].def->id == "view.grid");

    RowFilter text;
    text.text = "dissolve";
    CHECK(buildRows(f.registry, f.keymap, text).size() == 1);
    text.text = "ctrl x";   // words may be in the bindings
    CHECK(buildRows(f.registry, f.keymap, text).size() == 1);
    text.text = "mesh nothing";
    CHECK(buildRows(f.registry, f.keymap, text).empty());

    RowFilter cat;
    cat.category = "Edit";
    CHECK(buildRows(f.registry, f.keymap, cat).size() == 1);
    RowFilter ctx;
    ctx.context = "tool:box";
    rows = buildRows(f.registry, f.keymap, ctx);
    std::vector<std::string> ids;
    for (const auto& r : rows) ids.push_back(r.def->id);
    CHECK(ids == std::vector<std::string>{"mesh.dissolve", "file.save"});   // available in it (file.save everywhere)

    BindingEditor ed(f.registry, f.keymap);
    REQUIRE(ed.add("view.grid", "area:viewport", "G", nullptr).ok);
    RowFilter changed;
    changed.changed = true;
    rows = buildRows(f.registry, f.keymap, changed);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].def->id == "view.grid");
    CHECK(rows[0].bindings[0].fromUser);

    // two actions on one input in one context (both the user's)
    REQUIRE(ed.add("mesh.dissolve", "area:viewport", "G", nullptr, BindingEditor::Resolve::KeepBoth).ok);
    RowFilter conflicts;
    conflicts.conflicts = true;
    rows = buildRows(f.registry, f.keymap, conflicts);
    CHECK(rows.size() == 2);

    CHECK(categories(f.registry) == std::vector<std::string>{"Edit", "Mesh", "File", "View", "Camera"});
    CHECK(bindingText(bind("file.save", "global", "Ctrl+S")) == "Ctrl+S");
    CHECK(bindingText(bind("edit.delete", "area:viewport", "X")) == "X (area:viewport)");
    Binding withParams;
    REQUIRE(makeBinding("camera.mode", "area:viewport", "Numpad 7", nlohmann::json{{"mode", "top"}}, false, withParams));
    CHECK(bindingText(withParams) == "Numpad 7 [top] (area:viewport)");
    REQUIRE(makeBinding("transform.axis", "modal:transform", "Y", nlohmann::json{{"axis", 1}}, false, withParams));
    CHECK(bindingText(withParams) == "Y [1] (modal:transform)");
}

TEST_CASE("reverse lookup shows what an input does in every context") {
    Fixture f;
    auto l = lookupInput(f.registry, f.keymap, "ctrl+x");
    REQUIRE(l.size() == 1);
    CHECK(l[0].action == "mesh.dissolve");
    CHECK(l[0].label == "Dissolve");
    CHECK(l[0].context == "tool:box");
    BindingEditor ed(f.registry, f.keymap);
    REQUIRE(ed.add("file.save", "area:viewport", "Ctrl+X", nullptr, BindingEditor::Resolve::KeepBoth).ok);
    CHECK(lookupInput(f.registry, f.keymap, "Ctrl+X").size() == 2);
    CHECK(lookupInput(f.registry, f.keymap, "Ctrl+Alt+Q").empty());
    CHECK(lookupInput(f.registry, f.keymap, "not a chord ++").empty());
}

TEST_CASE("editing: conflict choices, remove, reset, undo and redo") {
    Fixture f;
    BindingEditor ed(f.registry, f.keymap);
    CHECK_FALSE(ed.canUndo());

    // bad input
    CHECK_FALSE(ed.add("nope", "global", "F5", nullptr).ok);
    CHECK_FALSE(ed.add("file.save", "global", "Ctrl+Meta+Q++", nullptr).ok);
    CHECK_FALSE(ed.add("file.save", "global", "Meta+S", nullptr).ok);
    CHECK_FALSE(ed.canUndo());

    // cancel on a conflict adds nothing and says what it clashed with
    auto r = ed.add("view.grid", "area:viewport", "X", nullptr);
    CHECK_FALSE(r.ok);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.error.find("Delete") != std::string::npos);
    CHECK(f.keymap.bindingsFor("view.grid").empty());
    CHECK_FALSE(ed.canUndo());

    // keep both cannot work against a preset binding of the same input in the same context
    r = ed.add("view.grid", "area:viewport", "X", nullptr, BindingEditor::Resolve::KeepBoth);
    CHECK_FALSE(r.ok);
    CHECK(f.keymap.userEmpty());
    CHECK(BindingEditor::canKeepBoth(conflictsFor(f.registry, f.keymap, bind("mesh.dissolve", "tool:box", "X"))));   // a shadowing context is fine
    CHECK_FALSE(BindingEditor::canKeepBoth(conflictsFor(f.registry, f.keymap, bind("view.grid", "area:viewport", "X"))));

    // replace: Delete loses X, Grid has it
    r = ed.add("view.grid", "area:viewport", "X", nullptr, BindingEditor::Resolve::Replace);
    CHECK(r.ok);
    CHECK(f.keymap.bindingsFor("view.grid").size() == 1);
    REQUIRE(f.keymap.bindingsFor("edit.delete").size() == 1);
    CHECK(f.keymap.bindingsFor("edit.delete")[0]->input == "Delete");
    CHECK(f.keymap.conflicts().empty());
    CHECK(ed.canUndo());

    // the same binding again changes nothing
    const int rev = ed.revision();
    CHECK(ed.add("view.grid", "area:viewport", "X", nullptr).ok);
    CHECK(ed.revision() == rev);

    // undo / redo
    REQUIRE(ed.undo());
    CHECK(f.keymap.bindingsFor("view.grid").empty());
    CHECK(f.keymap.bindingsFor("edit.delete").size() == 2);
    CHECK(f.keymap.userEmpty());
    CHECK(ed.canRedo());
    REQUIRE(ed.redo());
    CHECK(f.keymap.bindingsFor("view.grid").size() == 1);
    CHECK(f.keymap.bindingsFor("edit.delete").size() == 1);
    // a new change drops the redo list
    REQUIRE(ed.add("file.save", "global", "F2", nullptr).ok);
    CHECK_FALSE(ed.canRedo());

    // remove a preset binding and a user binding
    REQUIRE(ed.remove(bind("edit.delete", "area:viewport", "Delete")));
    CHECK(f.keymap.bindingsFor("edit.delete").empty());
    CHECK_FALSE(ed.remove(bind("edit.delete", "area:viewport", "Delete")));   // already gone
    REQUIRE(ed.remove(bind("file.save", "global", "F2")));
    CHECK(f.keymap.bindingsFor("file.save").size() == 1);

    // reset one action: its preset bindings come back, the chord Grid took over comes back too
    REQUIRE(ed.resetAction("edit.delete"));
    CHECK(f.keymap.bindingsFor("edit.delete").size() == 2);
    CHECK(f.keymap.bindingsFor("view.grid").empty());   // Grid's X was what hid Delete's X: gone with the reset
    CHECK_FALSE(ed.resetAction("edit.delete"));
    // reset all
    REQUIRE(ed.add("file.save", "global", "F3", nullptr).ok);
    REQUIRE(ed.resetAll());
    CHECK(f.keymap.userEmpty());
    CHECK_FALSE(ed.resetAll());
    // everything done can be undone, step by step
    int steps = 0;
    while (ed.undo()) ++steps;
    CHECK(steps >= 5);
    CHECK(f.keymap.userEmpty());
}

TEST_CASE("user keymap files: several, save, load, import and export") {
    Fixture f;
    TempDir dir;
    KeymapStore store(dir.path / "keymaps");
    CHECK(store.names().empty());
    CHECK(KeymapStore::checkName("mine").empty());
    CHECK(KeymapStore::checkName("My map 2.1-b").empty());
    CHECK_FALSE(KeymapStore::checkName("").empty());
    CHECK_FALSE(KeymapStore::checkName("../x").empty());
    CHECK_FALSE(KeymapStore::checkName("a/b").empty());
    CHECK_FALSE(KeymapStore::checkName(".hidden").empty());

    BindingEditor ed(f.registry, f.keymap);
    REQUIRE(ed.add("view.grid", "area:viewport", "Ctrl+Alt+G", nullptr).ok);
    REQUIRE(ed.remove(bind("edit.delete", "area:viewport", "X")));
    const auto effectiveOf = [](const Keymap& k) {
        std::vector<std::string> out;
        for (const Binding& b : k.effective()) out.push_back(b.action + "|" + b.context + "|" + b.input);
        return out;
    };
    const auto mine = effectiveOf(f.keymap);

    REQUIRE(store.save("mine", f.keymap).empty());   // the folder is created
    REQUIRE(store.save("other", Keymap{}).empty());
    CHECK(store.names() == std::vector<std::string>{"mine", "other"});

    // a clean profile loading it gets the same effective keymap
    Fixture clean;
    CHECK(effectiveOf(clean.keymap) != mine);
    CHECK(store.load("mine", clean.keymap).empty());
    CHECK(effectiveOf(clean.keymap) == mine);
    // a missing file is an empty layer
    CHECK(store.load("nothing", clean.keymap).empty());
    CHECK(clean.keymap.userEmpty());
    // a broken file is reported and leaves the keymap alone
    {
        std::ofstream(store.pathOf("broken")) << "{ not json";
    }
    REQUIRE(store.load("mine", clean.keymap).empty());
    CHECK_FALSE(store.load("broken", clean.keymap).empty());
    CHECK(effectiveOf(clean.keymap) == mine);

    // export, then import under another name on a clean profile
    const fs::path exported = dir.path / "share" / "team.json";
    REQUIRE(store.exportFile(exported, f.keymap).empty());
    CHECK(fs::exists(exported));
    KeymapStore other(dir.path / "other_profile");
    REQUIRE(other.importFile(exported, "team").empty());
    Fixture third;
    REQUIRE(other.load("team", third.keymap).empty());
    CHECK(effectiveOf(third.keymap) == mine);
    CHECK(readFile(other.pathOf("team")) == readFile(exported));
    // a file that is not a keymap, or has a bad chord, is refused and not stored
    {
        std::ofstream(dir.path / "bad.json") << R"({"bind": [{"action": "file.save", "context": "global", "input": "Ctrl+Nonsense++"}]})";
    }
    CHECK_FALSE(other.importFile(dir.path / "bad.json", "bad").empty());
    CHECK_FALSE(fs::exists(other.pathOf("bad")));
    CHECK_FALSE(other.importFile(dir.path / "missing.json", "x").empty());
    CHECK_FALSE(other.importFile(exported, "../escape").empty());

    CHECK(store.remove("other").empty());
    CHECK(store.names() == std::vector<std::string>{"broken", "mine"});
}

TEST_CASE("the real registry: every action can be rebound, rebinding applies at once, the default preset is clean") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string err;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &err), err);
    CHECK(keymap.conflicts().empty());   // the conflicts filter is empty for the default preset
    RowFilter conflicts;
    conflicts.conflicts = true;
    CHECK(buildRows(registry, keymap, conflicts).empty());

    // every action takes a binding in its own default context (camera navigation and modal operator keys included)
    BindingEditor ed(registry, keymap);
    const std::string probeInput = bind("file.exit", "global", "Ctrl+Alt+Shift+F12").input;   // the canonical spelling
    std::set<std::string> categoriesSeen;
    for (const ActionDef& d : registry.all()) {
        INFO("action " << d.id);
        const std::string context = defaultContextFor(d);
        const auto r = ed.add(d.id, context, "Ctrl+Alt+Shift+F12", d.params.is_null() ? nlohmann::json() : d.params, BindingEditor::Resolve::KeepBoth);
        CHECK(r.ok);
        categoriesSeen.insert(d.category);
        const auto have = keymap.bindingsFor(d.id);
        CHECK(std::any_of(have.begin(), have.end(), [&](const Binding* b) { return b->context == context && b->input == probeInput; }));
    }
    CHECK(categoriesSeen.size() >= 6);
    // the fly keys, a modal key and the box tool are among them
    CHECK(registry.find("camera.fly.forward"));
    CHECK(registry.find("transform.axis"));
    ed.resetAll();
    CHECK(keymap.userEmpty());

    // rebinding applies at the next event: Exit moves from Ctrl+Q to Ctrl+Alt+F9
    Router router(registry, keymap);
    ActionEnv env;
    CHECK(router.resolve(key("Q", kCtrl), env) == "file.exit");
    REQUIRE(ed.add("file.exit", "global", "Ctrl+Alt+F9", nullptr).ok);
    REQUIRE(ed.remove(Binding(*keymap.bindingsFor("file.exit")[0])));
    CHECK(router.resolve(key("Q", kCtrl), env).empty());
    CHECK(router.resolve(key("F9", kCtrl | kAlt), env) == "file.exit");
    // a fly key can be moved too (W -> I in the hold context); the actions that need a window are checked in the GUI test
    const auto fwd = keymap.bindingsFor("camera.fly.forward");
    REQUIRE_FALSE(fwd.empty());
    const Binding original = *fwd[0];
    REQUIRE(ed.remove(original));
    REQUIRE(ed.add("camera.fly.forward", original.context, "I", nullptr, BindingEditor::Resolve::Replace).ok);
    const auto moved = keymap.bindingsFor("camera.fly.forward");
    REQUIRE(moved.size() == 1);
    CHECK(moved[0]->input == "I");
    CHECK(moved[0]->context == original.context);
}

TEST_CASE("the default preset gives every action that exists a binding, except the ones that are menu or panel commands") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string err;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &err), err);
    RowFilter unbound;
    unbound.unbound = true;
    std::vector<std::string> ids;
    for (const ActionRow& r : buildRows(registry, keymap, unbound)) ids.push_back(r.def->id);
    std::sort(ids.begin(), ids.end());
    std::string all;
    for (const std::string& id : ids) all += id + " ";
    // what stays unbound on purpose: it is reached from a menu or a panel, or needs a parameter the user picks
    const std::set<std::string> allowed = {"file.open_recent", "file.export", "file.cancel_export", "file.import_quake_map", "file.set_assets_root", "grid.set", "view.hide_rule", "view.details.toggle", "view.surface.toggle", "view.textures.toggle", "view.outliner.toggle",
                                           "view.place.toggle", "view.stats.toggle", "view.render_mode", "view.dlss", "view.grid", "help.keys_toggle",
                                           "mesh.bridge", "mesh.smooth", "mesh.sharp", "mesh.subdivide", "brush.hollow", "brush.merge", "brush.snap", "brush.select_inside", "brush.select_touching",
                                           "texture.apply", "texture.lift", "texture.copy_uv", "texture.paste_uv", "texture.flow", "texture.hotspot", "texture.reset", "texture.fit", "texture.match_grid",
                                           "texture.justify", "texture.project", "texture.lightmap", "texture.properties"};
    for (const std::string& id : ids) {
        INFO("unbound: " << all);
        CHECK(allowed.count(id) == 1);
    }
}

TEST_CASE("recorded events become chord text") {
    using ui::ChordCapture;
    InputEvent k;
    k.type = InputEvent::Type::KeyPress;
    k.key = "S";
    k.scan = "S";
    k.mods = kCtrl | kShift;
    CHECK(ChordCapture::textFor(k, ChordMode::Press, false) == "Ctrl+Shift+S");
    CHECK(ChordCapture::textFor(k, ChordMode::Hold, false) == "Hold Ctrl+Shift+S");
    CHECK(ChordCapture::textFor(k, ChordMode::Release, false) == "Release Ctrl+Shift+S");
    CHECK(ChordCapture::textFor(k, ChordMode::DoubleTap, false) == "Double Ctrl+Shift+S");
    // a layout character with a US position: physical records the position
    k.key = "Ð«";
    k.scan = "S";
    k.mods = 0;
    CHECK(ChordCapture::textFor(k, ChordMode::Press, true) == "phys:S");
    // a key without a name of its own falls back to its position
    k.key.clear();
    k.scan = "1";
    k.mods = kShift;
    CHECK(ChordCapture::textFor(k, ChordMode::Press, false) == "Shift+1");
    k.scan.clear();
    CHECK(ChordCapture::textFor(k, ChordMode::Press, false).empty());
    // releases and moves are not recorded
    InputEvent m;
    m.type = InputEvent::Type::MouseMove;
    CHECK(ChordCapture::textFor(m, ChordMode::Press, false).empty());
    m.type = InputEvent::Type::MousePress;
    m.button = 3;
    CHECK(ChordCapture::textFor(m, ChordMode::Hold, false) == "MMB");   // the key mode does not apply to the mouse
    CHECK(ChordCapture::textFor(m, ChordMode::Press, false, true) == "MMB drag");
    m.button = 4;
    CHECK(ChordCapture::textFor(m, ChordMode::Press, false) == "Mouse4");
    InputEvent w;
    w.type = InputEvent::Type::Wheel;
    w.wheel = -100;   // away from the user = up
    w.mods = kCtrl;
    CHECK(ChordCapture::textFor(w, ChordMode::Press, false) == "Ctrl+Wheel Up");
    w.wheel = 100;
    CHECK(ChordCapture::textFor(w, ChordMode::Press, false) == "Ctrl+Wheel Down");
    w.wheel = 0;
    CHECK(ChordCapture::textFor(w, ChordMode::Press, false).empty());
    // what is recorded parses back
    for (const char* t : {"Ctrl+Shift+S", "Hold J", "Shift+LMB drag", "Ctrl+Wheel Up", "Mouse4", "phys:S"}) {
        Chord c;
        INFO(t);
        CHECK(parseChord(t, c));
    }
}
