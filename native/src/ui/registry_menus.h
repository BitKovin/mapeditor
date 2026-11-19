// ui/registry_menus -- builds the menu bar from the action registry.
//
// Owns: one QAction per menu entry of an action (or per variant of a parameterised action), grouped into menus by the action's menu
// path ("&View/&Layout"), with the binding text from the keymap shown right-aligned, and the enabled / checked state refreshed from the
// actions whenever a menu is about to open. Menu entries only call Registry::run; they never register shortcuts (the router owns keys).
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §2, 08_MILESTONES.md T1.6.
#pragma once

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QObject>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/actions/keymap.h"
#include "app/actions/registry.h"
#include "ui/app_actions.h"

namespace native::ui {

class RegistryMenus : public QObject {
public:
    // `env` returns the environment menu actions run with (the active viewport...). Registry, keymap and menu bar must outlive this.
    RegistryMenus(QMenuBar* bar, actions::Registry& registry, const actions::Keymap& keymap, std::function<UiEnv()> env, QObject* parent = nullptr);

    // Creates the menus and entries from the registry (call once, after every action is registered).
    void build();
    // Re-reads enabled / checked / binding text of every entry (done automatically when a menu opens).
    void refresh();
    // The menu at a path ("&View/&Layout"); null when no action put an entry there.
    QMenu* menu(const std::string& path) const;
    // The entry of an action (and params for a variant); null when none.
    QAction* entry(const std::string& actionId, const nlohmann::json& params = nullptr) const;

private:
    struct Entry {
        QAction* qaction = nullptr;
        std::string actionId;
        nlohmann::json params;
        std::string label;
    };
    QMenu* menuFor(const std::string& path);
    void fillDynamic(QMenu* menu, const std::string& actionId);

    QMenuBar* bar_;
    actions::Registry& registry_;
    const actions::Keymap& keymap_;
    std::function<UiEnv()> env_;
    std::map<std::string, QMenu*> menus_;
    std::vector<Entry> entries_;
};

} // namespace native::ui
