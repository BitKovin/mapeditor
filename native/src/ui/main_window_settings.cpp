// ui/main_window_settings -- the settings side of the main window (T4.5): the editor preferences, the recent list, the Settings dialog, the confirm question.
#include <QMessageBox>
#include <QSettings>
#include <QStringList>

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

} // namespace native::ui
