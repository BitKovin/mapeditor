// ui/level_settings_panel -- the Level dock (shell/src/ui/levelSettingsPanel.ts): light brightness and ambient light, colour correction, baked ambient occlusion, the quality falloff,
// export and visibility settings, the optimisation values and the bake quality, and the sky (Auto sky, margin, skybox). Every value is a worldspawn key (doc/level_settings): an empty
// field is the default (the placeholder shows it), the reset button removes the key, and every change is one undo step (MainWindow::setLevelKeys). A field that has the keyboard is
// not overwritten when the document changes.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md (levelSettingsPanel), 08_MILESTONES.md T11.10b.
#pragma once

#include <QWidget>

#include <array>
#include <string>

#include "doc/level_settings.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace native::ui {

class MainWindow;

class LevelSettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit LevelSettingsPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Reads the worldspawn again and fills every field (not the one that has the keyboard).
    void refreshNow();
    /// A named child widget: "field:<key>" (the line edit of a number key), "reset:<key>", "ambientColor", "ambientIntensity", "skyAuto", "skyMargin", "skyTexture", "skyChoose"; null when there is none.
    QWidget* part(const QString& name) const;

    // The edits of the fields (the widgets call these; tests too). Each is one undo step; false when nothing changed or the document is busy.
    bool setValue(const std::string& key, const QString& text);     // typed text of a number key: empty / the default removes it
    bool reset(const std::string& key);
    bool setAmbient(const std::array<double, 3>& color, double intensity);
    bool setAutoSky(native::doc::AutoSky mode);                      // on without a skybox picks the first one of env/ in the same step
    bool setSkyMargin(const QString& text);
    bool setSkyTexture(const QString& text);

private:
    void build();
    QWidget* numberRow(const native::doc::LevelField& field);
    QPushButton* resetButton(const std::string& key);
    void fillField(QLineEdit* edit, const native::doc::LevelField& field, const core::Entity* world);

    MainWindow& win_;
    QTimer* timer_ = nullptr;
    QComboBox* skyAuto_ = nullptr;
    QLineEdit *skyMargin_ = nullptr, *skyTexture_ = nullptr;
    QLineEdit* ambientIntensity_ = nullptr;
    QPushButton* ambientColor_ = nullptr;
    QLabel* skyWarn_ = nullptr;
    std::array<double, 3> ambientRgb_{1, 1, 1};
    bool filling_ = false;
};

} // namespace native::ui
