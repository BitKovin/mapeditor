// ui/select_actions -- see select_actions.h.
#include "ui/select_actions.h"

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

void add(actions::Registry& r, const char* id, const char* label, const char* description, bool separator, std::function<bool(MainWindow&)> enabled,
         std::function<void(MainWindow&, const json&)> run) {
    ActionDef d;
    d.id = id;
    d.label = label;
    d.category = "Edit";
    d.description = description;
    if (label && *label && separator) d.menuSeparatorBefore = true;
    if (std::string(id) != "edit.nudge") d.menu = "&Edit";
    d.contexts = {"area:viewport", "tool:select"};
    d.enabled = [enabled](const ActionEnv& env, const json&) {
        MainWindow* w = windowOf(env);
        return w && enabled(*w);
    };
    d.run = [run](ActionEnv& env, const Invocation& inv) {
        if (auto* w = windowOf(env)) run(*w, inv.params);
    };
    r.add(std::move(d));
}

} // namespace

void registerSelectActions(actions::Registry& r) {
    add(r, "edit.select_all", "Select all", "Selects every brush, point entity and prefab instance that is not hidden.", true,
        [](MainWindow& w) { return w.canSelectItems(); }, [](MainWindow& w, const json&) { w.selectAllItems(); });
    add(r, "edit.select_invert", "Invert selection", "Selects everything that is not selected now.", false,
        [](MainWindow& w) { return w.canSelectItems(); }, [](MainWindow& w, const json&) { w.invertSelectionItems(); });
    add(r, "edit.select_none", "Select none", "Clears the selection (Esc first leaves the origin of an entity when that is being edited).", false,
        [](MainWindow& w) { return w.canSelectItems() && !w.selection().empty() && w.originSelected().empty(); }, [](MainWindow& w, const json&) { w.deselectAll(); });

    {
        ActionDef d;
        d.id = "edit.select_similar";
        d.label = "Select similar";
        d.category = "Edit";
        d.description = "Selects everything that has the same class, texture or prefab file as the selection.";
        d.menu = "&Edit/Select &similar";
        d.contexts = {"area:viewport"};
        d.params = json{{"kind", "class"}};
        d.variants = {{"Same &class", {{"kind", "class"}}}, {"Same &texture", {{"kind", "texture"}}}, {"Same &prefab", {{"kind", "prefab"}}}};
        d.enabled = [](const ActionEnv& env, const json&) {
            MainWindow* w = windowOf(env);
            return w && w->canSelectItems() && !w->selection().empty();
        };
        d.run = [](ActionEnv& env, const Invocation& inv) {
            if (auto* w = windowOf(env)) w->selectSimilar(inv.params.is_object() ? inv.params.value("kind", std::string("class")) : "class");
        };
        r.add(std::move(d));
    }

    ActionDef d;
    d.id = "edit.nudge";
    d.label = "Nudge selection";
    d.category = "Edit";
    d.description = "Moves the selection one grid step (the arrows along the screen's ground axes, PageUp / PageDown up and down; fine = an eighth of the step).";
    d.contexts = {"area:viewport"};
    d.params = json{{"key", "left"}, {"fine", false}};
    d.variants = {{"&Left", {{"key", "left"}}}, {"&Right", {{"key", "right"}}}, {"&Forward", {{"key", "up"}}}, {"&Back", {{"key", "down"}}}, {"&Up", {{"key", "pageup"}}}, {"&Down", {{"key", "pagedown"}}}};
    d.enabled = [](const ActionEnv& env, const json&) {
        const MainWindow* w = windowOf(env);
        return w && w->canNudge();
    };
    d.run = [](ActionEnv& env, const Invocation& inv) {
        if (auto* w = windowOf(env)) w->nudgeSelected(inv.params.is_object() ? inv.params.value("key", std::string("left")) : "left", inv.params.is_object() && inv.params.value("fine", false));
    };
    r.add(std::move(d));
}

} // namespace native::ui
