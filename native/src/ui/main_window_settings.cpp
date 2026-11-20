// ui/main_window_settings -- the settings side of the main window (T4.5): the editor preferences, the recent list, the Settings dialog, the confirm question.
#include <QMessageBox>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <filesystem>

#include "app/actions/keymap_edit.h"
#include "base/log.h"
#include "tools/prefs.h"
#include "ui/main_window.h"
#include "ui/registry_menus.h"
#include "ui/settings_dialog.h"

namespace native::ui {

void MainWindow::setEditorPrefs(const tools::EditorPrefs& prefs) {
    const tools::EditorPrefs clean = tools::sanitize(prefs);
    if (clean == prefs_) return;
    prefs_ = clean;
    if (services_.settings) services_.settings->setValue("prefs/editor", QString::fromStdString(tools::prefsToJson(prefs_)));
    emit editorPrefsChanged();
}

void MainWindow::clearRecentFiles() {
    recent_.clear();
    if (services_.settings) services_.settings->setValue("files/recent", QStringList());
    if (menus()) menus()->refresh();
}

bool MainWindow::confirm(const QString& title, const QString& text) {
    if (confirmHandler_) return confirmHandler_(title, text);
    return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void MainWindow::openSettings(int page) {
    SettingsDialog dialog(*this);
    dialog.showPage(page);
    dialog.exec();
}

// ---- keymaps (T4.6) ----------------------------------------------------------------------------------------------------------------

void MainWindow::initKeymapFiles() {
    namespace fs = std::filesystem;
    keymapDir_.clear();
    keymapName_ = "default";
    if (services_.userKeymapPath.empty()) return;
    const fs::path file(services_.userKeymapPath);
    keymapDir_ = file.parent_path().generic_string();
    if (keymapDir_.empty()) keymapDir_ = ".";
    keymapName_ = file.stem().string();
    if (services_.settings) {
        const std::string saved = services_.settings->value("keymap/active").toString().toStdString();
        if (!saved.empty() && actions::KeymapStore::checkName(saved).empty()) keymapName_ = saved;
    }
    if (!actions::KeymapStore::checkName(keymapName_).empty()) keymapName_ = "default";
}

std::string MainWindow::activeKeymapPath() const {
    if (keymapDir_.empty()) return {};
    return actions::KeymapStore(keymapDir_).pathOf(keymapName_).generic_string();
}

std::vector<std::string> MainWindow::keymapNames() const {
    if (keymapDir_.empty()) return {};
    std::vector<std::string> names = actions::KeymapStore(keymapDir_).names();
    if (std::find(names.begin(), names.end(), keymapName_) == names.end()) names.push_back(keymapName_);
    std::sort(names.begin(), names.end());
    return names;
}

void MainWindow::keymapEdited() {
    if (!keymapDir_.empty() && !saveUserKeymap()) base::logError("keymap: cannot save " + activeKeymapPath());
    if (menus()) menus()->refresh();
    emit keymapChanged();
}

std::string MainWindow::switchKeymap(const std::string& name) {
    if (keymapDir_.empty()) return "There is no keymap folder.";
    if (const std::string bad = actions::KeymapStore::checkName(name); !bad.empty()) return bad;
    if (name == keymapName_) return {};
    const actions::KeymapStore store(keymapDir_);
    actions::Keymap probe = keymap_;
    if (const std::string err = store.load(name, probe); !err.empty()) return err;   // a broken file is refused, the current layer stays
    if (!keymap_.userEmpty() || std::filesystem::exists(store.pathOf(keymapName_))) saveUserKeymap();
    keymap_ = std::move(probe);
    keymapName_ = name;
    if (services_.settings) services_.settings->setValue("keymap/active", QString::fromStdString(name));
    if (menus()) menus()->refresh();
    emit keymapChanged();
    return {};
}

std::string MainWindow::newKeymap(const std::string& name) {
    if (keymapDir_.empty()) return "There is no keymap folder.";
    if (const std::string bad = actions::KeymapStore::checkName(name); !bad.empty()) return bad;
    const actions::KeymapStore store(keymapDir_);
    if (std::filesystem::exists(store.pathOf(name)) || name == keymapName_) return "A keymap named \"" + name + "\" exists already.";
    if (!keymap_.userEmpty() || std::filesystem::exists(store.pathOf(keymapName_))) saveUserKeymap();
    keymap_.clearUser();
    keymapName_ = name;
    if (const std::string err = store.save(name, keymap_); !err.empty()) return err;
    if (services_.settings) services_.settings->setValue("keymap/active", QString::fromStdString(name));
    if (menus()) menus()->refresh();
    emit keymapChanged();
    return {};
}

std::string MainWindow::deleteKeymap(const std::string& name) {
    if (keymapDir_.empty()) return "There is no keymap folder.";
    const actions::KeymapStore store(keymapDir_);
    if (const std::string err = store.remove(name); !err.empty()) return err;
    if (name == keymapName_) {
        keymap_.clearUser();
        keymapName_ = "default";
        if (const std::string err = store.load(keymapName_, keymap_); !err.empty()) return err;
        if (services_.settings) services_.settings->setValue("keymap/active", QString::fromStdString(keymapName_));
        if (menus()) menus()->refresh();
        emit keymapChanged();
    }
    return {};
}

std::string MainWindow::importKeymap(const std::string& file, const std::string& name) {
    if (keymapDir_.empty()) return "There is no keymap folder.";
    const actions::KeymapStore store(keymapDir_);
    if (const std::string err = store.importFile(file, name); !err.empty()) return err;
    if (name == keymapName_) {   // replacing the active layer: read it again
        if (const std::string err = store.load(name, keymap_); !err.empty()) return err;
        if (menus()) menus()->refresh();
        emit keymapChanged();
        return {};
    }
    return switchKeymap(name);
}

std::string MainWindow::exportKeymap(const std::string& file) const { return actions::KeymapStore(keymapDir_.empty() ? "." : keymapDir_).exportFile(file, keymap_); }

} // namespace native::ui
