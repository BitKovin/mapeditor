// T1.6: action registry, keymap layers and the input router (Qt-free, no GPU).
#include <doctest.h>

#include <string>
#include <vector>

#include "app/actions/contexts.h"
#include "app/actions/input.h"
#include "app/actions/keymap.h"
#include "app/actions/registry.h"
#include "app/actions/router.h"

using namespace native::actions;
using nlohmann::json;

namespace {

// The environment of the tests: a log of what ran.
struct Log {
    std::vector<std::string> ran;
    std::vector<std::string> phases;
    std::vector<json> params;
    bool undoEnabled = true;
    int moves = 0;
};

ActionDef def(const std::string& id, ActionKind kind, Log& log, std::vector<std::string> contexts = {}, std::string holdContext = {}) {
    ActionDef d;
    d.id = id;
    d.label = id;
    d.kind = kind;
    d.contexts = std::move(contexts);
    d.holdContext = std::move(holdContext);
    d.run = [&log, id](ActionEnv&, const Invocation& inv) {
        log.ran.push_back(id);
        log.params.push_back(inv.params);
        switch (inv.event.type) {
            case InputEvent::Type::MouseMove: log.phases.push_back(id + ":move"); ++log.moves; break;
            case InputEvent::Type::KeyRelease:
            case InputEvent::Type::MouseRelease: log.phases.push_back(id + ":release"); break;
            default: log.phases.push_back(id + ":press"); break;
        }
    };
    return d;
}

InputEvent key(const std::string& k, uint8_t mods = 0, int64_t t = 0, const std::string& scan = {}) {
    InputEvent e;
    e.type = InputEvent::Type::KeyPress;
    e.key = k;
    e.scan = scan.empty() ? k : scan;
    e.mods = mods;
    e.timeMs = t;
    return e;
}
InputEvent keyUp(const std::string& k, uint8_t mods = 0, int64_t t = 0) {
    InputEvent e = key(k, mods, t);
    e.type = InputEvent::Type::KeyRelease;
    return e;
}
InputEvent mouse(InputEvent::Type type, int button, uint8_t mods = 0, int64_t t = 0) {
    InputEvent e;
    e.type = type;
    e.button = button;
    e.mods = mods;
    e.timeMs = t;
    return e;
}
InputEvent move(double dx, double dy, int64_t t = 0) {
    InputEvent e;
    e.type = InputEvent::Type::MouseMove;
    e.dx = dx;
    e.dy = dy;
    e.timeMs = t;
    return e;
}
InputEvent wheel(double delta, uint8_t mods = 0) {
    InputEvent e;
    e.type = InputEvent::Type::Wheel;
    e.wheel = delta;
    e.mods = mods;
    return e;
}

bool bind(Keymap& km, const std::string& action, const std::string& context, const std::string& input, json params = nullptr, bool ignoreShift = false) {
    // Test keymaps are built through the user layer on an empty preset.
    Binding b;
    std::string err;
    if (!makeBinding(action, context, input, params, ignoreShift, b, &err)) {
        MESSAGE("bad binding: " << err);
        return false;
    }
    km.bindUser(b);
    return true;
}

} // namespace

TEST_CASE("chords parse and print canonically") {
    const char* const forms[] = {"Ctrl+Shift+S", "Ctrl+K, Ctrl+S", "Hold Space", "Release Alt", "Double Shift", "Shift+LMB", "Ctrl+RMB drag",
                                 "MMB drag", "Mouse4", "Wheel Up", "Ctrl+Wheel", "phys:W", "Numpad .", "F5", "Delete", "Alt+N", "Ctrl+,",
                                 "Wheel Down", "Ctrl+Alt+Shift+Meta+X"};
    for (const char* f : forms) {
        Chord c;
        std::string err;
        INFO(f);
        REQUIRE(parseChord(f, c, &err));
        Chord again;
        REQUIRE(parseChord(formatChord(c), again));
        CHECK(again == c);
    }
    Chord c;
    REQUIRE(parseChord("ctrl+shift+s", c));
    CHECK(formatChord(c) == "Ctrl+Shift+S");
    REQUIRE(parseChord("shift+ctrl+s", c));
    CHECK(formatChord(c) == "Ctrl+Shift+S");               // modifier order is canonical
    REQUIRE(parseChord("esc", c));
    CHECK(formatChord(c) == "Escape");
    REQUIRE(parseChord("RMB drag", c));
    CHECK(c.mode == ChordMode::Hold);                      // a drag is a hold
    CHECK(c.steps[0].drag);
    REQUIRE(parseChord("Ctrl+K, Ctrl+S", c));
    CHECK(c.steps.size() == 2);
    REQUIRE(parseChord("phys:w", c));
    CHECK(c.steps[0].physical);
    CHECK(c.steps[0].key == "W");
    REQUIRE(parseChord("Ctrl+Wheel", c));
    CHECK(c.steps[0].wheelDir == 0);

    std::string err;
    CHECK_FALSE(parseChord("", c, &err));
    CHECK_FALSE(parseChord("Ctrl+Banana", c, &err));
    CHECK(err.find("Banana") != std::string::npos);
    CHECK_FALSE(parseChord("A, B, C", c));                 // at most 2 steps
    CHECK_FALSE(parseChord("Hold A, B", c));               // only a plain press is a sequence
    CHECK_FALSE(parseChord("Hold Wheel Up", c));
}

TEST_CASE("registry: add, find, aliases, run") {
    Log log;
    Registry reg;
    CHECK(reg.add(def("file.save", ActionKind::Trigger, log)));
    CHECK_FALSE(reg.add(def("file.save", ActionKind::Trigger, log)));   // ids are unique
    CHECK_FALSE(reg.add(ActionDef{}));                                  // and not empty
    reg.addAlias("file.store", "file.save");
    REQUIRE(reg.find("file.store") != nullptr);
    CHECK(reg.find("file.store")->id == "file.save");
    CHECK(reg.find("nope") == nullptr);
    ActionEnv env;
    CHECK(reg.run("file.store", env));
    CHECK(log.ran.size() == 1);
    CHECK_FALSE(reg.run("nope", env));
    // Parameterised: the call's parameters override the default ones.
    ActionDef g = def("grid.set", ActionKind::Trigger, log);
    g.params = {{"size", 1}};
    g.enabled = [&log](const ActionEnv&, const json&) { return log.undoEnabled; };
    reg.add(g);
    reg.run("grid.set", env);
    CHECK(log.params.back() == json({{"size", 1}}));
    reg.run("grid.set", env, json({{"size", 0.25}}));
    CHECK(log.params.back() == json({{"size", 0.25}}));
    log.undoEnabled = false;
    CHECK_FALSE(reg.run("grid.set", env));                 // disabled actions do not run
}

TEST_CASE("the same input resolves to different actions in different contexts; a specific context shadows a general one") {
    Log log;
    Registry reg;
    reg.add(def("object.delete", ActionKind::Trigger, log));
    reg.add(def("mesh.dissolve", ActionKind::Trigger, log, {"mode:mesh.edge", "mode:mesh.vertex"}));
    reg.add(def("clipper.cancel", ActionKind::Trigger, log, {"tool:clipper"}));
    Keymap km;
    REQUIRE(bind(km, "object.delete", "area:viewport", "X"));
    REQUIRE(bind(km, "mesh.dissolve", "mode:mesh.edge", "X"));
    REQUIRE(bind(km, "clipper.cancel", "tool:clipper", "X"));
    Router r(reg, km);
    ActionEnv env;

    // Only the viewport is active: X deletes.
    r.contexts().set({"window:main", "area:viewport"});
    CHECK(r.handle(key("X"), env).action == "object.delete");
    // Mesh edge mode inside the viewport: the same X dissolves (more specific context).
    r.contexts().set({"window:main", "area:viewport", "mode:mesh.edge"});
    CHECK(r.handle(key("X"), env).action == "mesh.dissolve");
    // A clipper tool in mesh mode shadows both.
    r.contexts().set({"window:main", "area:viewport", "mode:mesh.edge", "tool:clipper"});
    CHECK(r.handle(key("X"), env).action == "clipper.cancel");
    // Outside the viewport nothing is bound to X.
    r.contexts().set({"window:main", "area:outliner"});
    CHECK_FALSE(r.handle(key("X"), env).handled);
    // Reverse lookup answers without running anything.
    log.ran.clear();
    r.contexts().set({"window:main", "area:viewport", "mode:mesh.edge"});
    CHECK(r.resolve(key("X"), env) == "mesh.dissolve");
    CHECK(log.ran.empty());
    // Dissolve is only available in mesh vertex / edge modes: in face mode its binding does not apply.
    r.contexts().set({"window:main", "area:viewport", "mode:mesh.face"});
    CHECK(r.handle(key("X"), env).action == "object.delete");
}

TEST_CASE("a disabled specific action falls through to the general one") {
    Log log;
    Registry reg;
    reg.add(def("general", ActionKind::Trigger, log));
    ActionDef specific = def("specific", ActionKind::Trigger, log);
    specific.enabled = [&log](const ActionEnv&, const json&) { return log.undoEnabled; };
    reg.add(specific);
    Keymap km;
    bind(km, "general", "area:viewport", "G");
    bind(km, "specific", "tool:grab", "G");
    Router r(reg, km);
    ActionEnv env;
    r.contexts().set({"area:viewport", "tool:grab"});
    CHECK(r.handle(key("G"), env).action == "specific");
    log.undoEnabled = false;
    CHECK(r.handle(key("G"), env).action == "general");
}

TEST_CASE("a user keymap adds and removes bindings over the preset") {
    Log log;
    Registry reg;
    reg.add(def("mesh.bevel", ActionKind::Trigger, log));
    reg.add(def("mesh.merge", ActionKind::Trigger, log));
    reg.add(def("grid.set", ActionKind::Trigger, log));
    Keymap km;
    std::string err;
    REQUIRE(km.loadPreset(R"({"name":"default","bindings":[
        {"action":"mesh.bevel","context":"mode:mesh.edge","input":"Ctrl+B"},
        {"action":"mesh.merge","context":"mode:mesh.vertex","input":"M"}]})", &err));
    CHECK(km.presetName() == "default");
    CHECK(km.effective().size() == 2);
    Router r(reg, km);
    ActionEnv env;
    r.contexts().set({"area:viewport", "mode:mesh.vertex"});
    CHECK(r.handle(key("M"), env).action == "mesh.merge");

    // The user removes M, adds a parameterised grid binding and rebinds Ctrl+B.
    REQUIRE(km.loadUser(R"({"base":"default","version":1,
        "bind":[{"action":"grid.set","params":{"size":0.25},"context":"area:viewport","input":"Alt+1"},
                {"action":"mesh.bevel","context":"mode:mesh.edge","input":"Shift+B"}],
        "unbind":[{"action":"mesh.merge","context":"mode:mesh.vertex","input":"M"}]})", &err));
    CHECK_FALSE(r.handle(key("M"), env).handled);
    log.ran.clear();
    CHECK(r.handle(key("1", kAlt), env).action == "grid.set");
    CHECK(log.params.back() == json({{"size", 0.25}}));       // the binding's parameters reach the action
    r.contexts().set({"area:viewport", "mode:mesh.edge"});
    CHECK(r.handle(key("B", kCtrl), env).action == "mesh.bevel");   // the preset binding is still there
    CHECK(r.handle(key("B", kShift), env).action == "mesh.bevel");  // and so is the user's extra one
    CHECK(km.effective().size() == 3);

    // The user layer saves only the differences and loads back to the same effective keymap.
    const std::string saved = km.userToJson();
    const json j = json::parse(saved);
    CHECK(j["bind"].size() == 2);
    CHECK(j["unbind"].size() == 1);
    Keymap km2;
    REQUIRE(km2.loadPreset(R"({"name":"default","bindings":[
        {"action":"mesh.bevel","context":"mode:mesh.edge","input":"Ctrl+B"},
        {"action":"mesh.merge","context":"mode:mesh.vertex","input":"M"}]})"));
    REQUIRE(km2.loadUser(saved));
    CHECK(km2.effective().size() == km.effective().size());

    // Binding an input that the preset uses in the same context takes it over; clearing the user layer brings the preset back.
    REQUIRE(bind(km, "grid.set", "mode:mesh.edge", "Ctrl+B"));
    CHECK(r.handle(key("B", kCtrl), env).action == "grid.set");
    km.clearUser();
    CHECK(km.userEmpty());
    CHECK(r.handle(key("B", kCtrl), env).action == "mesh.bevel");
    r.contexts().set({"area:viewport", "mode:mesh.vertex"});
    CHECK(r.handle(key("M"), env).action == "mesh.merge");

    // unbindUser / bindUser round trip on a preset binding.
    Binding m;
    REQUIRE(makeBinding("mesh.merge", "mode:mesh.vertex", "M", nullptr, false, m));
    CHECK(km.unbindUser(m));
    CHECK_FALSE(km.unbindUser(m));                         // already removed
    CHECK_FALSE(r.handle(key("M"), env).handled);
    km.bindUser(m);
    CHECK(km.userEmpty());                                 // re-binding a removed preset binding just restores it
    CHECK(r.handle(key("M"), env).action == "mesh.merge");

    // A broken user file is rejected and leaves the layer untouched.
    CHECK_FALSE(km.loadUser(R"({"bind":[{"action":"x","input":"Ctrl+Banana"}]})", &err));
    CHECK(err.find("Banana") != std::string::npos);
    CHECK_FALSE(km.loadUser("not json"));
    CHECK_FALSE(km.loadPreset(R"({"bindings":[{"input":"A"}]})", &err));
}

TEST_CASE("Ctrl+Z works with a Cyrillic layout active (US key position)") {
    Log log;
    Registry reg;
    reg.add(def("edit.undo", ActionKind::Trigger, log));
    reg.add(def("tool.grab", ActionKind::Trigger, log));
    reg.add(def("camera.fly.forward", ActionKind::Hold, log));
    Keymap km;
    bind(km, "edit.undo", "global", "Ctrl+Z");
    bind(km, "tool.grab", "area:viewport", "G");
    bind(km, "camera.fly.forward", "area:viewport", "Hold phys:W", nullptr, true);
    Router r(reg, km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport"});
    // Russian layout: the key at the US "Z" position produces "Я", at "G" produces "П", at "W" produces "Ц".
    CHECK(r.handle(key("Я", kCtrl, 0, "Z"), env).action == "edit.undo");
    CHECK(r.handle(key("П", 0, 0, "G"), env).action == "tool.grab");
    // A logical Latin key on a Latin layout works as before; a French AZERTY "W" key is the physical Z position but a logical W.
    CHECK(r.handle(key("Z", kCtrl, 0, "Z"), env).action == "edit.undo");
    CHECK(r.handle(key("G", 0, 0, "G"), env).action == "tool.grab");
    // AZERTY: the key that produces "Z" sits at the US "W" position. Ctrl+Z is logical, so it still undoes; the physical W binding
    // (fly forward) is the one at that position.
    CHECK(r.handle(key("Z", kCtrl, 0, "W"), env).action == "edit.undo");
    CHECK(r.handle(key("Z", 0, 0, "W"), env).action == "camera.fly.forward");
    // Physical bindings follow the position on every layout, including Cyrillic.
    r.cancelAll(env);
    CHECK(r.handle(key("Ц", 0, 0, "W"), env).action == "camera.fly.forward");
}

TEST_CASE("key sequences complete within 1 s and time out after it") {
    Log log;
    Registry reg;
    reg.add(def("file.save_all", ActionKind::Trigger, log));
    reg.add(def("edit.comment", ActionKind::Trigger, log));
    Keymap km;
    bind(km, "file.save_all", "global", "Ctrl+K, Ctrl+S");
    bind(km, "edit.comment", "global", "Ctrl+K, Ctrl+C");
    Router r(reg, km);
    ActionEnv env;
    // The first step is consumed; the second runs the action.
    auto res = r.handle(key("K", kCtrl, 0), env);
    CHECK(res.handled);
    CHECK(res.action.empty());
    CHECK(r.sequencePending());
    CHECK(log.ran.empty());
    CHECK(r.handle(key("S", kCtrl, 900), env).action == "file.save_all");
    CHECK_FALSE(r.sequencePending());
    // The other second step of the same prefix.
    r.handle(key("K", kCtrl, 2000), env);
    CHECK(r.handle(key("C", kCtrl, 2500), env).action == "edit.comment");
    // After 1 s the prefix is forgotten: the second key is an ordinary key press that is bound to nothing.
    log.ran.clear();
    r.handle(key("K", kCtrl, 5000), env);
    CHECK(r.sequencePending());
    CHECK_FALSE(r.handle(key("S", kCtrl, 6001), env).handled);
    CHECK(log.ran.empty());
    // A wrong second key cancels the sequence too.
    r.handle(key("K", kCtrl, 7000), env);
    CHECK_FALSE(r.handle(key("X", kCtrl, 7100), env).handled);
    CHECK_FALSE(r.sequencePending());
    CHECK_FALSE(r.handle(key("S", kCtrl, 7200), env).handled);
}

TEST_CASE("a focused text field gets the letters; Ctrl chords still reach the router") {
    Log log;
    Registry reg;
    reg.add(def("tool.grab", ActionKind::Trigger, log));
    reg.add(def("edit.undo", ActionKind::Trigger, log));
    reg.add(def("view.focus", ActionKind::Trigger, log));
    Keymap km;
    bind(km, "tool.grab", "global", "G");
    bind(km, "edit.undo", "global", "Ctrl+Z");
    bind(km, "view.focus", "global", "Alt+F");
    Router r(reg, km);
    ActionEnv env;
    InputEvent g = key("G");
    g.textFieldFocused = true;
    CHECK_FALSE(r.handle(g, env).handled);                 // typing a "g" into a field
    InputEvent shiftG = key("G", kShift);
    shiftG.textFieldFocused = true;
    CHECK_FALSE(r.handle(shiftG, env).handled);
    InputEvent z = key("Z", kCtrl);
    z.textFieldFocused = true;
    CHECK(r.handle(z, env).action == "edit.undo");         // Ctrl chords are not typing
    InputEvent af = key("F", kAlt);
    af.textFieldFocused = true;
    CHECK(r.handle(af, env).action == "view.focus");
    CHECK(r.handle(key("G"), env).action == "tool.grab");  // without the field it is the tool key
    CHECK(log.ran.size() == 3);
}

TEST_CASE("holds: a mouse drag runs its action, pushes its hold context, and fly keys apply only inside it") {
    Log log;
    Registry reg;
    reg.add(def("camera.look", ActionKind::Hold, log, {"view:perspective"}, "hold:camera.fly"));
    reg.add(def("camera.fly.forward", ActionKind::Hold, log, {"hold:camera.fly"}));
    reg.add(def("camera.fly.speed", ActionKind::Axis, log, {"hold:camera.fly"}));
    reg.add(def("camera.dolly", ActionKind::Axis, log, {"area:viewport"}));
    reg.add(def("tool.select_all", ActionKind::Trigger, log));
    Keymap km;
    bind(km, "camera.look", "view:perspective", "RMB drag");
    bind(km, "camera.fly.forward", "hold:camera.fly", "Hold phys:W", nullptr, true);
    bind(km, "camera.fly.speed", "hold:camera.fly", "Wheel");
    bind(km, "camera.dolly", "area:viewport", "Wheel");
    bind(km, "tool.select_all", "global", "W");           // a logical W elsewhere
    Router r(reg, km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport", "view:perspective"});

    // W outside a hold is the global tool key, the wheel dollies.
    CHECK(r.handle(key("W"), env).action == "tool.select_all");
    CHECK(r.handle(wheel(-100), env).action == "camera.dolly");

    log.phases.clear();
    CHECK(r.handle(mouse(InputEvent::Type::MousePress, 2), env).action == "camera.look");
    CHECK(r.holding("camera.look"));
    CHECK(r.contextActive("hold:camera.fly"));
    r.handle(move(3, 4), env);
    r.handle(move(1, 0), env);
    // Inside the hold: W flies (the physical hold binding is more specific than the global tool key) and the wheel changes the speed.
    CHECK(r.handle(key("W", kShift), env).action == "camera.fly.forward");       // Shift held: ignoreShift
    CHECK(r.holding("camera.fly.forward"));
    CHECK(r.handle(wheel(-100), env).action == "camera.fly.speed");
    // Key repeat of the held key is swallowed and does not run anything again.
    const size_t before = log.ran.size();
    InputEvent rep = key("W", kShift);
    rep.autoRepeat = true;
    CHECK(r.handle(rep, env).handled);
    CHECK(log.ran.size() == before);
    // Releasing the key and the button end the holds, the hold context disappears, and the old behaviour is back.
    CHECK(r.handle(keyUp("W"), env).action == "camera.fly.forward");
    CHECK_FALSE(r.holding("camera.fly.forward"));
    CHECK(r.handle(mouse(InputEvent::Type::MouseRelease, 2), env).action == "camera.look");
    CHECK_FALSE(r.holding());
    CHECK_FALSE(r.contextActive("hold:camera.fly"));
    CHECK(r.handle(wheel(-100), env).action == "camera.dolly");
    CHECK(r.handle(key("W"), env).action == "tool.select_all");
    // Phases, in order: press, two moves, key hold press, wheel, key release, button release.
    const std::vector<std::string> expected = {"camera.look:press", "camera.look:move", "camera.look:move", "camera.fly.forward:press",
                                               "camera.fly.speed:press", "camera.fly.forward:release", "camera.look:release",
                                               "camera.dolly:press", "tool.select_all:press"};
    CHECK(log.phases == expected);
    // Moves with no hold are not consumed.
    CHECK_FALSE(r.handle(move(5, 5), env).handled);
}

TEST_CASE("cancelAll ends every hold (focus lost) and its context") {
    Log log;
    Registry reg;
    reg.add(def("camera.look", ActionKind::Hold, log, {}, "hold:camera.fly"));
    reg.add(def("camera.boost", ActionKind::Hold, log));
    Keymap km;
    bind(km, "camera.look", "global", "RMB drag");
    bind(km, "camera.boost", "global", "Hold Shift");
    Router r(reg, km);
    ActionEnv env;
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    r.handle(key("Shift"), env);
    CHECK(r.holding("camera.look"));
    CHECK(r.holding("camera.boost"));
    r.cancelAll(env);
    CHECK_FALSE(r.holding());
    CHECK_FALSE(r.contextActive("hold:camera.fly"));
    CHECK(log.phases.back().find("release") != std::string::npos);
    // A release that arrives later is harmless.
    CHECK_FALSE(r.handle(keyUp("Shift"), env).handled);
}

TEST_CASE("mouse chords, double tap and release bindings") {
    Log log;
    Registry reg;
    reg.add(def("select.replace", ActionKind::Trigger, log));
    reg.add(def("select.add", ActionKind::Trigger, log));
    reg.add(def("select.face", ActionKind::Trigger, log));
    reg.add(def("view.toggle_x", ActionKind::Trigger, log));
    reg.add(def("temp.end", ActionKind::Trigger, log));
    Keymap km;
    bind(km, "select.replace", "area:viewport", "LMB");
    bind(km, "select.add", "area:viewport", "Ctrl+LMB");
    bind(km, "select.face", "area:viewport", "Shift+LMB");
    bind(km, "view.toggle_x", "global", "Double Shift");
    bind(km, "temp.end", "global", "Release Alt");
    Router r(reg, km);
    ActionEnv env;
    r.contexts().set({"area:viewport"});
    CHECK(r.handle(mouse(InputEvent::Type::MousePress, 1), env).action == "select.replace");
    CHECK(r.handle(mouse(InputEvent::Type::MousePress, 1, kCtrl), env).action == "select.add");
    CHECK(r.handle(mouse(InputEvent::Type::MousePress, 1, kShift), env).action == "select.face");
    CHECK_FALSE(r.handle(mouse(InputEvent::Type::MousePress, 3), env).handled);   // MMB is not bound here
    CHECK_FALSE(r.handle(mouse(InputEvent::Type::MousePress, 1, kAlt), env).handled);   // exact modifiers

    // Double tap: two Shift presses within 300 ms; a slow second tap does not count.
    log.ran.clear();
    r.handle(key("Shift", 0, 1000), env);
    r.handle(keyUp("Shift", 0, 1050), env);
    CHECK(r.handle(key("Shift", 0, 1200), env).action == "view.toggle_x");
    r.handle(keyUp("Shift", 0, 1250), env);
    CHECK(r.handle(key("Shift", 0, 1400), env).action.empty());                   // the tap after a double starts a new one
    r.handle(keyUp("Shift", 0, 1450), env);
    r.handle(key("Shift", 0, 2000), env);
    CHECK_FALSE(r.handle(key("Shift", 0, 2400), env).handled);                    // 400 ms: too slow
    CHECK(std::count(log.ran.begin(), log.ran.end(), "view.toggle_x") == 1);

    // Release binding: fires when Alt goes up.
    CHECK(r.handle(keyUp("Alt"), env).action == "temp.end");
}

TEST_CASE("conflicts in a keymap are reported") {
    Log log;
    Keymap km;
    REQUIRE(km.loadPreset(R"({"name":"p","bindings":[
        {"action":"a","context":"area:viewport","input":"X"},
        {"action":"b","context":"area:viewport","input":"X"},
        {"action":"c","context":"mode:mesh.edge","input":"X"},
        {"action":"a","context":"area:viewport","input":"Delete"}]})"));
    const auto c = km.conflicts();
    REQUIRE(c.size() == 1);
    CHECK(c[0].first->action == "a");
    CHECK(c[0].second->action == "b");
    CHECK(km.bindingsFor("a").size() == 2);
    CHECK(km.displayText("a") == "X");
    CHECK(km.displayText("nope").empty());
}

// ---- input that must never get stuck ----------------------------------------------------------------------------------------------

namespace {

struct LookRig {
    Log log;
    Registry reg;
    Keymap km;
    LookRig() {
        reg.add(def("camera.look", ActionKind::Hold, log, {"view:perspective"}, "hold:camera.fly"));
        reg.add(def("camera.fly.forward", ActionKind::Hold, log, {"hold:camera.fly"}));
        bind(km, "camera.look", "view:perspective", "RMB drag");
        bind(km, "camera.fly.forward", "hold:camera.fly", "Hold phys:W", nullptr, true);
    }
};

} // namespace

TEST_CASE("holds: the fly keys end with the look button, so no direction stays pressed without it") {
    LookRig rig;
    Router r(rig.reg, rig.km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport", "view:perspective"});
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    r.handle(key("W"), env);
    REQUIRE(r.holding("camera.fly.forward"));
    rig.log.phases.clear();
    r.handle(mouse(InputEvent::Type::MouseRelease, 2), env);
    CHECK_FALSE(r.holding());
    CHECK(rig.log.phases == std::vector<std::string>{"camera.look:release", "camera.fly.forward:release"});   // the key was released too
    // the (late) key release finds nothing to end and does not run anything twice
    rig.log.phases.clear();
    r.handle(keyUp("W"), env);
    CHECK(rig.log.phases.empty());
}

TEST_CASE("holds: a key release always ends the hold, even when a text field has the keyboard") {
    LookRig rig;
    Router r(rig.reg, rig.km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport", "view:perspective"});
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    r.handle(key("W"), env);
    REQUIRE(r.holding("camera.fly.forward"));
    InputEvent up = keyUp("W");
    up.textFieldFocused = true;
    CHECK(r.handle(up, env).handled);
    CHECK_FALSE(r.holding("camera.fly.forward"));
    CHECK(r.holding("camera.look"));
}

TEST_CASE("holds: release(trigger) ends a hold whose release never arrived; heldTriggers names them") {
    LookRig rig;
    Router r(rig.reg, rig.km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport", "view:perspective"});
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    r.handle(key("W"), env);
    CHECK(r.heldTriggers() == std::vector<std::string>{"btn:2", "key:W"});
    CHECK(r.release("key:W", env));
    CHECK_FALSE(r.holding("camera.fly.forward"));
    CHECK_FALSE(r.release("key:W", env));
    CHECK(r.release(Router::mouseTrigger(2), env));
    CHECK_FALSE(r.holding());
    CHECK_FALSE(r.contextActive("hold:camera.fly"));
}

TEST_CASE("holds: a pointer move that says the button is up ends the drag instead of continuing it") {
    LookRig rig;
    Router r(rig.reg, rig.km);
    ActionEnv env;
    r.contexts().set({"window:main", "area:viewport", "view:perspective"});
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    InputEvent m = move(5, 5);
    m.buttonsKnown = true;
    m.buttons = 1u << 1;   // the right button is down: the drag goes on
    r.handle(m, env);
    CHECK(r.holding("camera.look"));
    rig.log.phases.clear();
    m.buttons = 0;         // the system says no button is down: the release was lost
    CHECK(r.handle(m, env).handled);
    CHECK_FALSE(r.holding());
    CHECK(rig.log.phases == std::vector<std::string>{"camera.look:release"});
    // moves that carry no button information never end anything
    r.handle(mouse(InputEvent::Type::MousePress, 2), env);
    r.handle(move(1, 1), env);
    CHECK(r.holding("camera.look"));
}
