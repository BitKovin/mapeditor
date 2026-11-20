// ui/place_panel -- the Place dock (Unreal's Place Actors): Shapes (the shape to draw with the box tool and its parameters, deferred from T4.2), Prefabs (every
// *.map.json under the profile's prefab folder), Entities (the point classes of the game's .ent, or a starter list) and Lights. A click on an entity, light or prefab
// adds it where the camera looks (resting on the surface in the middle of the view) and selects it.
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <QWidget>

#include <map>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTabWidget;
class QTimer;
class QToolButton;

namespace native::ui {

class MainWindow;

class PlacePanel : public QWidget {
    Q_OBJECT
public:
    explicit PlacePanel(MainWindow& window, QWidget* parent = nullptr);
    ~PlacePanel() override;

    QTabWidget* tabs() const { return tabs_; }
    QListWidget* entityList() const { return entities_; }
    QListWidget* lightList() const { return lights_; }
    QListWidget* prefabList() const { return prefabs_; }
    // Looks at the prefab folder again / rebuilds the entity and light lists from the definitions.
    void rescanPrefabs();
    void rebuildClasses();
    // The prefab files found (assets-relative, sorted).
    const std::vector<std::string>& prefabFiles() const { return prefabFiles_; }
    // The panel's shape widgets follow the window (the tool or its parameters changed).
    void syncShape();

private:
    void buildShapes(QWidget* page);
    void fillEntityList(QListWidget* list, const std::vector<std::string>& classes, const QString& filter);
    void drawPrefabs();
    void applyParams();

    MainWindow& win_;
    QTabWidget* tabs_ = nullptr;
    QListWidget *entities_ = nullptr, *lights_ = nullptr, *prefabs_ = nullptr;
    QLineEdit *entityFilter_ = nullptr, *prefabFilter_ = nullptr;
    QLabel* prefabInfo_ = nullptr;
    std::vector<QToolButton*> shapeButtons_;
    QSpinBox *sides_ = nullptr, *steps_ = nullptr, *segments_ = nullptr, *rings_ = nullptr, *archSegments_ = nullptr;
    QDoubleSpinBox* archThickness_ = nullptr;
    QCheckBox* smooth_ = nullptr;
    QComboBox* turn_ = nullptr;
    QLabel* shapeHint_ = nullptr;
    QWidget* paramRows_[8] = {};
    std::vector<std::string> prefabFiles_;
    std::vector<std::string> entityClasses_, lightClasses_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> keyvalues_;   // extra keyvalues a starter entity is added with
    bool syncing_ = false;
};

} // namespace native::ui
