// app/actions/registry -- every thing the user can do is an action; menus, toolbars and key help are generated from this list.
//
// Owns: ActionDef (id, label, kind, contexts, enabled / checked / run callbacks, menu path), the Registry that stores them, looks
// them up by id (with aliases for renamed ids) and runs them, and ActionEnv, the opaque application state the callbacks use.
// Qt-free. Modules register their own actions (tools/mesh registers mesh.*); from T1.6 on no widget or tool handles a key itself.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §2, 08_MILESTONES.md T1.6.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/actions/input.h"

namespace native::actions {

enum class ActionKind {
    Trigger,   // runs once on press
    Toggle,    // on / off state shown in menus (checked() says which)
    Hold,      // active while the input is held: run() is called on press, on every move and on release
    Axis,      // continuous: mouse move, wheel
    Modal,     // keys inside a running modal operator
};

// The application state the callbacks need, passed through the router untouched. The application defines what `app` points to
// (ui::MainWindow glue sets a struct holding the viewport an event belongs to); callbacks cast it back with as<T>().
struct ActionEnv {
    void* app = nullptr;
    template <class T>
    T* as() const { return static_cast<T*>(app); }
};

// What a run() callback is told: the event that triggered it (a synthetic KeyPress for menus / the command palette) and the
// parameters of the binding or menu entry.
struct Invocation {
    InputEvent event;
    nlohmann::json params;
};

// One menu entry of a parameterised action ("1 viewport" = view.layout {count: 1}).
struct MenuVariant {
    std::string label;
    nlohmann::json params;
};

struct ActionDef {
    std::string id;                  // stable, dotted: "file.save", "camera.fly.forward"; never changes once shipped
    std::string label;               // "Save", "Fly forward"
    std::string category;            // "File", "Camera" ...: groups actions in the binding editor / palette
    std::string description;
    ActionKind kind = ActionKind::Trigger;
    std::vector<std::string> contexts;   // where it is available (10 §3); empty = everywhere
    std::string menu;                // "View/Layout" menu path the entry is generated into; empty = not in a menu
    std::string menuLabel;           // entry text when the menu shows it (defaults to label)
    // The entry text computed each time the menu is refreshed ("Undo Move"); an empty result keeps the label.
    std::function<std::string(const ActionEnv&)> dynamicLabel;
    std::vector<MenuVariant> variants;   // one menu entry per variant (own label and parameters) instead of a single entry
    bool menuSeparatorBefore = false;    // a separator is drawn above this entry
    // The menu path is a submenu whose entries are made each time it opens (recent files): label + params of each entry.
    std::function<std::vector<MenuVariant>(const ActionEnv&)> dynamicVariants;
    std::string holdContext;         // Hold: context that is active while the hold runs ("hold:camera.fly")
    nlohmann::json params;           // default parameters (a binding / menu entry may override them)
    std::function<bool(const ActionEnv&, const nlohmann::json&)> enabled;   // null = always
    std::function<void(ActionEnv&, const Invocation&)> run;
    std::function<bool(const ActionEnv&, const nlohmann::json&)> checked;   // Toggle only
    std::string icon;
};

class Registry {
public:
    // False (and nothing registered) when the id is empty or already taken.
    bool add(ActionDef def);
    // An old id that keeps working after a rename.
    void addAlias(const std::string& oldId, const std::string& newId);

    const ActionDef* find(const std::string& id) const;   // resolves aliases
    const std::vector<ActionDef>& all() const { return actions_; }
    size_t size() const { return actions_.size(); }

    bool isEnabled(const ActionDef& def, const ActionEnv& env, const nlohmann::json& params) const;
    bool isChecked(const ActionDef& def, const ActionEnv& env, const nlohmann::json& params) const;
    // Runs an action by id as a menu / palette / panel button would (no key involved). False when unknown or disabled.
    bool run(const std::string& id, ActionEnv& env, const nlohmann::json& params = nullptr);

private:
    std::vector<ActionDef> actions_;
    std::map<std::string, size_t> index_;
    std::map<std::string, std::string> aliases_;
};

} // namespace native::actions
