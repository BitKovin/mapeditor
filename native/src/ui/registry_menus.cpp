// ui/registry_menus -- see registry_menus.h.
#include "ui/registry_menus.h"

#include <QString>

namespace native::ui {

RegistryMenus::RegistryMenus(QMenuBar* bar, actions::Registry& registry, const actions::Keymap& keymap, std::function<UiEnv()> env, QObject* parent)
    : QObject(parent), bar_(bar), registry_(registry), keymap_(keymap), env_(std::move(env)) {}

QMenu* RegistryMenus::menuFor(const std::string& path) {
    auto it = menus_.find(path);
    if (it != menus_.end()) return it->second;
    const size_t slash = path.rfind('/');
    QMenu* menu = nullptr;
    if (slash == std::string::npos) {
        menu = bar_->addMenu(QString::fromStdString(path));
    } else {
        QMenu* parent = menuFor(path.substr(0, slash));
        menu = parent->addMenu(QString::fromStdString(path.substr(slash + 1)));
    }
    QObject::connect(menu, &QMenu::aboutToShow, this, [this] { refresh(); });
    menus_[path] = menu;
    return menu;
}

QMenu* RegistryMenus::menu(const std::string& path) const {
    auto it = menus_.find(path);
    return it == menus_.end() ? nullptr : it->second;
}

QAction* RegistryMenus::entry(const std::string& actionId, const nlohmann::json& params) const {
    for (const Entry& e : entries_)
        if (e.actionId == actionId && (params.is_null() || e.params == params)) return e.qaction;
    return nullptr;
}

void RegistryMenus::build() {
    for (const actions::ActionDef& def : registry_.all()) {
        if (def.menu.empty()) continue;
        QMenu* menu = menuFor(def.menu);
        if (def.menuSeparatorBefore) menu->addSeparator();
        if (def.dynamicVariants) {
            // The path is a submenu refilled whenever it is about to open.
            const std::string id = def.id;
            QObject::connect(menu, &QMenu::aboutToShow, this, [this, menu, id] { fillDynamic(menu, id); });
            continue;
        }
        std::vector<actions::MenuVariant> variants = def.variants;
        if (variants.empty()) variants.push_back({def.menuLabel.empty() ? def.label : def.menuLabel, def.params});
        for (const actions::MenuVariant& v : variants) {
            Entry e;
            e.actionId = def.id;
            e.params = v.params.is_null() ? def.params : v.params;
            e.label = v.label;
            e.qaction = menu->addAction(QString::fromStdString(v.label));
            e.qaction->setCheckable(def.kind == actions::ActionKind::Toggle);
            const std::string id = def.id;
            const nlohmann::json params = e.params;
            QObject::connect(e.qaction, &QAction::triggered, this, [this, id, params] {
                UiEnv ui = env_();
                actions::ActionEnv env;
                env.app = &ui;
                registry_.run(id, env, params);
                refresh();
            });
            entries_.push_back(std::move(e));
        }
    }
    refresh();
}

void RegistryMenus::fillDynamic(QMenu* menu, const std::string& id) {
    const actions::ActionDef* def = registry_.find(id);
    if (!def || !def->dynamicVariants) return;
    UiEnv ui = env_();
    actions::ActionEnv env;
    env.app = &ui;
    menu->clear();
    const std::vector<actions::MenuVariant> variants = def->dynamicVariants(env);
    for (const actions::MenuVariant& v : variants) {
        QAction* a = menu->addAction(QString::fromStdString(v.label));
        const nlohmann::json params = v.params;
        QObject::connect(a, &QAction::triggered, this, [this, id, params] {
            UiEnv u = env_();
            actions::ActionEnv e;
            e.app = &u;
            registry_.run(id, e, params);
            refresh();
        });
    }
    if (variants.empty()) menu->addAction(QStringLiteral("(none)"))->setEnabled(false);
}

void RegistryMenus::refresh() {
    UiEnv ui = env_();
    actions::ActionEnv env;
    env.app = &ui;
    for (const Entry& e : entries_) {
        const actions::ActionDef* def = registry_.find(e.actionId);
        if (!def) continue;
        e.qaction->setEnabled(registry_.isEnabled(*def, env, e.params));
        if (e.qaction->isCheckable()) e.qaction->setChecked(registry_.isChecked(*def, env, e.params));
        // The binding is shown right-aligned after a tab; it is only text (no Qt shortcut is registered).
        const std::string keys = keymap_.displayText(e.actionId, def->variants.empty() ? nlohmann::json(nullptr) : e.params);
        e.qaction->setText(QString::fromStdString(e.label) + (keys.empty() ? QString() : QStringLiteral("\t") + QString::fromStdString(keys)));
    }
}

} // namespace native::ui
