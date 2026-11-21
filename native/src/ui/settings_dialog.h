// ui/settings_dialog -- Settings: General (the game folder), Game profile (game_profile.json of that folder), Editor (what new viewports start with, the
// recent maps) and Lighting (lighting.json of that folder: what a BSP export bakes with). The Keymap page (T4.6, the binding editor) applies its changes at once; further pages come with their features. OK / Apply write everything that changed:
//   Editor     -> MainWindow::setEditorPrefs (the QSettings)
//   Profile    -> game_profile.json through core::writeProfileFile, then MainWindow::reloadProfile
//   Lighting   -> lighting.json through tools::writeLightingConfig (only the keys that changed)
//   General    -> MainWindow::setAssetsRoot (profile, entity definitions, textures, prefabs and the panels follow)
// The profile goes first so it is written to the folder it was read from when both change in one Apply.
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.5; shell/src/ui/settingsDialog.ts.
#pragma once

#include <QDialog>

#include <map>
#include <string>

#include "exporting/launch.h"
#include "tools/lighting_config.h"
#include "tools/prefs.h"
#include "tools/profile_form.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace native::ui {

class KeymapPage;
class MainWindow;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum Page { General = 0, GameProfile = 1, Editor = 2, Lighting = 3, Keymap = 4 };

    explicit SettingsDialog(MainWindow& window, QWidget* parent = nullptr);

    // The binding editor page (its changes apply at once; Apply and OK do not touch it).
    KeymapPage* keymapPage() const { return keymap_; }
    void showPage(int page);
    int page() const;
    // Whether anything differs from what is stored.
    bool dirty() const;
    // Writes everything that changed. Empty = done (the pages show the stored values again), else the first reason (also shown in the dialog); a page that
    // fails stays as typed and the pages after it are not applied.
    QString applyAll();
    QString error() const;
    // Re-reads every page from the window / the files (the game folder changed, or the profile did).
    void reload();

private:
    void buildGeneral(QWidget* page);
    void buildProfile(QWidget* page);
    void buildEditor(QWidget* page);
    void buildLighting(QWidget* page);
    void reloadGeneral();
    void reloadProfile();
    void reloadEditor();
    void reloadLighting();
    tools::ProfileForm readProfileForm() const;
    tools::EditorPrefs readEditorPrefs() const;
    QString applyEditor();
    QString applyLighting();
    nlohmann::json lightingChanges() const;   // the keys whose editors differ from the stored file
    std::string lightingFile() const;
    void setLightingEdit(const std::string& key, const nlohmann::json& value);
    void resetLighting(const std::string& key);
    QString applyProfile();
    QString applyGeneral();
    void setError(const QString& text);
    void browseRoot();
    void useViewportValues();

    MainWindow& win_;
    QListWidget* pages_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* error_ = nullptr;
    // General
    QLineEdit* root_ = nullptr;
    QLabel* rootInfo_ = nullptr;
    QLabel* files_ = nullptr;
    QLineEdit *gameExe_ = nullptr, *gameArgs_ = nullptr, *gameDir_ = nullptr;
    exporting::LaunchSettings launchOf() const;
    // Game profile
    QWidget* profilePage_ = nullptr;
    QLabel* profileNote_ = nullptr;
    QLineEdit *name_ = nullptr, *textures_ = nullptr, *maps_ = nullptr, *prefabs_ = nullptr, *texGroups_ = nullptr, *entities_ = nullptr;
    QLineEdit *texExt_ = nullptr, *modelExt_ = nullptr;
    QDoubleSpinBox* ppm_ = nullptr;
    QComboBox* exporter_ = nullptr;
    QPlainTextEdit *block_ = nullptr, *receive_ = nullptr;
    tools::ProfileForm profileOriginal_;
    bool profileLoaded_ = false;
    // Editor
    QComboBox *grid_ = nullptr, *rot_ = nullptr, *scale_ = nullptr;
    QCheckBox *snap_ = nullptr, *showGrid_ = nullptr;
    QDoubleSpinBox* fly_ = nullptr;
    QLabel* recentInfo_ = nullptr;
    QPushButton* clearRecent_ = nullptr;
    // Lighting (lighting.json of the game folder; one editor per key, each remembers its default)
    QWidget* lightingPage_ = nullptr;
    QLabel* lightingNote_ = nullptr;
    std::map<std::string, QWidget*> lightingEdits_;
    tools::LightingConfig lightingOriginal_;
    bool lightingLoaded_ = false;
    // Keymap
    KeymapPage* keymap_ = nullptr;
};

} // namespace native::ui
