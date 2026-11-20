// T4.7: the command palette model, the status bar hints and the key help sections (app/actions/palette). Headless.
#include <doctest.h>

#include <QFile>

#include <algorithm>

#include "app/actions/keymap_edit.h"
#include "app/actions/palette.h"
#include "ui/app_actions.h"

using namespace native;
using namespace native::actions;
using json = nlohmann::json;

namespace {

std::string readAsset(const char* relative) {
    QFile f(QString::fromUtf8(NATIVE_ASSETS_DIR) + "/" + QString::fromUtf8(relative));
    REQUIRE_MESSAGE(f.open(QIODevice::ReadOnly), "cannot open ", f.fileName().toStdString());
    return f.readAll().toStdString();
}

ActionDef def(const std::string& id, const std::string& label, const std::string& category, ActionKind kind = ActionKind::Trigger) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = category;
    d.kind = kind;
    return d;
}

struct Fixture {
    Registry registry;
    Keymap keymap;
    bool saveEnabled = true;
    Fixture() {
        ActionDef save = def("file.save", "Save", "File");
        save.enabled = [this](const ActionEnv&, const json&) { return saveEnabled; };
        registry.add(save);
        registry.add(def("file.save_as", "Save as...", "File"));
        registry.add(def("edit.undo", "Undo", "Edit"));
        ActionDef smaller = def("grid.smaller", "Smaller grid", "Grid");
        registry.add(smaller);
        ActionDef set = def("grid.set", "Set grid size", "Grid");
        set.params = json{{"size", 0.5}};
        set.paletteKeyword = "grid";
        set.paletteParam = "size";
        set.paletteNumeric = true;
        registry.add(set);
        ActionDef mode = def("camera.mode", "Camera mode", "Camera", ActionKind::Toggle);
        mode.params = json{{"mode", "fly"}};
        mode.variants = {{"&Fly", {{"mode", "fly"}}}, {"&Top", {{"mode", "top"}}}};
        registry.add(mode);
        registry.add(def("camera.look", "Look", "Camera", ActionKind::Hold));
        registry.add(def("view.command_palette", "Command palette...", "View"));
        const char* preset = R"({"name": "t", "bindings": [
            {"action": "file.save", "context": "global", "input": "Ctrl+S"},
            {"action": "camera.mode", "context": "global", "input": "Numpad 7", "params": {"mode": "top"}}
        ]})";
        std::string err;
        REQUIRE_MESSAGE(keymap.loadPreset(preset, &err), err);
    }
    std::vector<PaletteEntry> entries() const { return paletteEntries(registry, keymap, ActionEnv{}); }
};

bool has(const std::vector<PaletteEntry>& list, const std::string& id, const json& params = nullptr) {
    return std::any_of(list.begin(), list.end(), [&](const PaletteEntry& e) { return e.id == id && e.params == params; });
}

} // namespace

TEST_CASE("fuzzy score: in order, case-insensitive, word starts and runs count") {
    CHECK(fuzzyScore("sg", "Set grid size") > 0);
    CHECK(fuzzyScore("SAVE", "Save as") > 0);
    CHECK(fuzzyScore("szg", "Set grid size") < 0);   // wrong order
    CHECK(fuzzyScore("zzz", "Save") < 0);
    CHECK(fuzzyScore("sa", "Save") > fuzzyScore("sa", "Pause as"));   // a word start beats the middle of a word
    CHECK(fuzzyScore("save", "Save") > fuzzyScore("save", "Set a vertex"));   // a run beats scattered letters
    CHECK(fuzzyScore("", "anything") == 0);
}

TEST_CASE("palette entries: enabled triggers and toggles, one per variant, with their keys") {
    Fixture f;
    auto list = f.entries();
    CHECK(has(list, "file.save"));
    CHECK(has(list, "camera.mode", json{{"mode", "fly"}}));
    CHECK(has(list, "camera.mode", json{{"mode", "top"}}));
    CHECK_FALSE(has(list, "camera.look"));               // a hold needs the mouse
    CHECK_FALSE(has(list, "view.command_palette"));      // it is already open
    const auto top = std::find_if(list.begin(), list.end(), [](const PaletteEntry& e) { return e.id == "camera.mode" && e.params == json{{"mode", "top"}}; });
    REQUIRE(top != list.end());
    CHECK(top->label == "Camera mode: Top");
    CHECK(top->keys == "Numpad 7");
    const auto save = std::find_if(list.begin(), list.end(), [](const PaletteEntry& e) { return e.id == "file.save"; });
    CHECK(save->keys == "Ctrl+S");
    CHECK(save->category == "File");
    f.saveEnabled = false;
    CHECK_FALSE(has(f.entries(), "file.save"));          // only what can run now
}

TEST_CASE("palette search: any word order, best first, recently used first on an empty query") {
    Fixture f;
    const auto list = f.entries();
    auto r = paletteSearch(list, f.registry, "save", {});
    REQUIRE(r.size() >= 2);
    CHECK(r[0].id == "file.save");           // the exact word beats "Save as..."
    CHECK(r[1].id == "file.save_as");
    r = paletteSearch(list, f.registry, "as save", {});
    REQUIRE_FALSE(r.empty());
    CHECK(r[0].id == "file.save_as");        // the words match in any order
    r = paletteSearch(list, f.registry, "camera top", {});
    REQUIRE_FALSE(r.empty());
    CHECK(r[0].params == json{{"mode", "top"}});   // the category counts as text
    CHECK(paletteSearch(list, f.registry, "qqq", {}).empty());

    r = paletteSearch(list, f.registry, "", {});
    CHECK(r.size() == list.size());
    r = paletteSearch(list, f.registry, "", {{"edit.undo", nullptr}, {"camera.mode", json{{"mode", "top"}}}});
    REQUIRE(r.size() >= 2);
    CHECK(r[0].id == "edit.undo");
    CHECK(r[1].id == "camera.mode");
    CHECK(r[1].params == json{{"mode", "top"}});
    CHECK(r[2].category <= r.back().category);   // then by category
}

TEST_CASE("palette search: an argument typed after the name runs the action with it") {
    Fixture f;
    const auto list = f.entries();
    auto r = paletteSearch(list, f.registry, "grid 0.25", {});
    REQUIRE_FALSE(r.empty());
    CHECK(r[0].id == "grid.set");
    CHECK(r[0].params == json{{"size", 0.25}});
    CHECK(r[0].label == "Set grid size: 0.25");
    r = paletteSearch(list, f.registry, "  Grid   2  ", {});
    REQUIRE_FALSE(r.empty());
    CHECK(r[0].params == json{{"size", 2.0}});
    // not a number: no typed entry, the words still search
    r = paletteSearch(list, f.registry, "grid abc", {});
    CHECK((r.empty() || r[0].params.is_null()));
}

TEST_CASE("every enabled palette entry can be found by its own label") {
    Fixture f;
    const auto list = f.entries();
    for (const PaletteEntry& e : list) {
        INFO("entry " << e.label);
        const auto r = paletteSearch(list, f.registry, e.label, {});
        CHECK(std::any_of(r.begin(), r.end(), [&](const PaletteEntry& m) { return m.id == e.id && m.params == e.params; }));
    }
}

TEST_CASE("the recently used list: moves to the front, is limited, survives bad settings text") {
    std::vector<Recent> list;
    list = recentUsed(list, {"a", nullptr});
    list = recentUsed(list, {"b", json{{"mode", "top"}}});
    list = recentUsed(list, {"a", nullptr});
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == "a");
    CHECK(list[1].id == "b");
    for (int i = 0; i < 20; ++i) list = recentUsed(list, {"x" + std::to_string(i), nullptr}, 5);
    CHECK(list.size() == 5);
    CHECK(list[0].id == "x19");
    const auto back = recentFromJson(recentToJson(list));
    CHECK(back == list);
    CHECK(recentFromJson("not json").empty());
    CHECK(recentFromJson("{\"id\": 3}").empty());
    CHECK(recentFromJson("[{\"id\": 3}, {\"id\": \"ok\"}]").size() == 1);
}

TEST_CASE("status bar hints come from the bindings of the active mode and change with them") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string err;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &err), err);

    const std::string modal = hintFor(registry, keymap, "modal:transform");
    CHECK(modal.find("LMB / Enter confirm") != std::string::npos);
    CHECK(modal.find("RMB / Escape cancel") != std::string::npos);
    CHECK(modal.find("X / Y / Z constrain") != std::string::npos);
    CHECK(modal.find("Numpad Enter") == std::string::npos);
    const std::string box = hintFor(registry, keymap, "tool:box");
    CHECK(box.find("T turn shape") != std::string::npos);
    CHECK(box.find("LMB drag") != std::string::npos);
    CHECK(box.find("draw") != std::string::npos);
    CHECK(box.find("Escape cancel") != std::string::npos);
    const std::string select = hintFor(registry, keymap, "tool:select");
    CHECK(select.find("LMB drag") != std::string::npos);
    CHECK(select.find("select") != std::string::npos);
    CHECK(hintFor(registry, keymap, "no:such:context").empty());

    // rebinding Escape (cancel) to F8 changes the hint at once
    BindingEditor ed(registry, keymap);
    const auto cancel = keymap.bindingsFor("transform.cancel");
    REQUIRE_FALSE(cancel.empty());
    const auto esc = std::find_if(cancel.begin(), cancel.end(), [](const Binding* b) { return b->input == "Escape"; });
    REQUIRE(esc != cancel.end());
    const Binding escBinding = **esc;
    REQUIRE(ed.remove(escBinding));
    REQUIRE(ed.add("transform.cancel", "modal:transform", "F8", nullptr).ok);
    const std::string changed = hintFor(registry, keymap, "modal:transform");
    CHECK(changed.find("RMB / F8 cancel") != std::string::npos);
    CHECK(changed.find("Escape") == std::string::npos);
    ed.undo();
    ed.undo();
    CHECK(hintFor(registry, keymap, "modal:transform") == modal);
}

TEST_CASE("key help sections: the most specific context first, global last, each action once with its inputs joined") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string err;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &err), err);
    const auto sections = keyHelpSections(registry, keymap, {"window:main", "area:viewport", "view:perspective", "view:fly", "tool:box"});
    REQUIRE(sections.size() >= 4);
    CHECK(sections.front().context == "tool:box");
    CHECK(sections.back().context == "global");
    const auto viewport = std::find_if(sections.begin(), sections.end(), [](const HelpSection& s) { return s.context == "area:viewport"; });
    REQUIRE(viewport != sections.end());
    int deletes = 0;
    for (const HelpLine& l : viewport->lines)
        if (l.label == "Delete") {
            ++deletes;
            CHECK(l.input == "Delete / Backspace");
        }
    CHECK(deletes == 1);
    // contexts that are not active do not appear
    CHECK(std::none_of(sections.begin(), sections.end(), [](const HelpSection& s) { return s.context == "modal:transform" || s.context == "tool:select"; }));
    const auto modal = keyHelpSections(registry, keymap, {"window:main", "modal:transform"});
    CHECK(modal.front().context == "modal:transform");
}

TEST_CASE("the palette, the key help and its hold are bound in the default preset") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string err;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &err), err);
    CHECK(keymap.displayText("view.command_palette") == "F3");
    const auto b = keymap.bindingsFor("view.command_palette");
    CHECK(b.size() == 2);
    const auto help = keymap.bindingsFor("help.keys");
    REQUIRE(help.size() == 1);
    CHECK(help[0]->input == "Hold F1");
    REQUIRE(registry.find("help.keys"));
    CHECK(registry.find("help.keys")->kind == ActionKind::Hold);
}
