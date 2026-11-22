// ui/main_toolbar -- the one tool bar under the menu bar (14_UI_REDESIGN.md section 6.2): the frequent actions as grouped icon buttons, built from the action registry, so a button
// runs the same action as the menu and the key, shows its binding in the tooltip, and follows the action's enabled / checked state. Groups: file / history, tools, edit mode,
// gizmo, snapping (grid and rotation steps), UV lock, viewport layout; at the right end the search field (the command palette) and Play.
// Owns: the QActions and the two step combos. Thread: UI thread.
// Spec: 14_UI_REDESIGN.md section 6.2, 08_MILESTONES.md T14.3.
#pragma once

#include <QToolBar>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class QAction;
class QComboBox;
class QToolButton;

namespace native::ui {

class MainWindow;

class MainToolBar : public QToolBar {
    Q_OBJECT
public:
    explicit MainToolBar(MainWindow& window);

    // Re-reads enabled / checked / tooltip of every button and the step combos (done on the window's change signals).
    void refresh();

    // The button of a registry action (and params for a variant); null when the toolbar has none.
    QAction* find(const std::string& actionId, const nlohmann::json& params = nullptr) const;
    // The two combos: grid size and rotation step of the active viewport.
    QComboBox* gridCombo() const { return grid_; }
    QComboBox* rotationCombo() const { return rotation_; }
    // The edit-mode buttons (not registry actions: they enter and leave the mesh mode).
    QAction* objectModeAction() const { return object_; }
    QAction* meshModeAction() const { return mesh_; }
    // The element buttons that only exist in mesh mode.
    const std::vector<QAction*>& elementActions() const { return elements_; }

protected:
    void changeEvent(QEvent* e) override;

private:
    struct Item {
        QAction* qaction = nullptr;
        std::string actionId;
        nlohmann::json params;
        std::string icon;
    };
    QAction* addRegistry(const std::string& actionId, const nlohmann::json& params, const std::string& icon, const QString& text = QString());
    void buildCombo(QComboBox* box, const std::vector<double>& values, const QString& unit);
    void refreshIcons();

    MainWindow& win_;
    std::vector<Item> items_;
    QComboBox* grid_ = nullptr;
    QComboBox* rotation_ = nullptr;
    QAction* object_ = nullptr;
    QAction* mesh_ = nullptr;
    std::vector<QAction*> elements_;
    QToolButton* shapeButton_ = nullptr;
    QAction* shapeAction_ = nullptr;
    QToolButton* search_ = nullptr;
    bool syncing_ = false;
};

} // namespace native::ui
