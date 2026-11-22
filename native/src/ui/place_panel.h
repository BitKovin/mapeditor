// ui/place_panel -- the Place dock (Unreal's Place Actors): Shapes (the shape to draw with the box tool and its parameters, deferred from T4.2), Prefabs (every
// *.map.json under the profile's prefab folder), Entities (the point classes of the game's .ent, or a starter list) and Lights. A click on an entity, light or prefab
// adds it where the camera looks (resting on the surface in the middle of the view) and selects it.
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <QIcon>
#include <QTimer>
#include <QWidget>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QSpinBox;
class QTabWidget;
class QToolButton;

namespace native::tools {
class PrefabPreviews;
}

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
    // The prefab pictures (T11.7b): one prefab is read and drawn per timer tick after the list was filled, so a long list never blocks the window; a prefab that cannot be
    // read shows why in its tooltip. `renderPrefabIcons` does the waiting ones now (tests); -1 = all, else at most that many; returns how many it did.
    int renderPrefabIcons(int limit = -1);
    int pendingPrefabIcons() const { return static_cast<int>(iconQueue_.size()); }
    QIcon prefabIcon(const std::string& file) const;
    // The panel's shape widgets follow the window (the tool or its parameters changed).
    void syncShape();
    // The shape buttons and the orientation box name their key from the keymap ("Cylinder (C): drag ..."); follows a rebinding.
    void refreshTooltips();
    QString shapeTooltip(size_t index) const { return static_cast<qsizetype>(index) < shapeTips_.size() ? shapeTips_[static_cast<qsizetype>(index)] : QString(); }

private:
    void buildShapes(QWidget* page);
    void buildDecals(QWidget* page);
    void syncDecals();
    void fillEntityList(QListWidget* list, const std::vector<std::string>& classes, const QString& filter);
    void drawPrefabs();
    void makeNextIcon();
    void applyIcon(const std::string& file);
    void applyParams();

    MainWindow& win_;
    QTabWidget* tabs_ = nullptr;
    QListWidget *entities_ = nullptr, *lights_ = nullptr, *prefabs_ = nullptr;
    QLineEdit *entityFilter_ = nullptr, *prefabFilter_ = nullptr;
    QLabel* prefabInfo_ = nullptr;
    std::vector<QToolButton*> shapeButtons_;
    QStringList shapeTips_;
    QSpinBox *sides_ = nullptr, *steps_ = nullptr, *segments_ = nullptr, *rings_ = nullptr, *archSegments_ = nullptr;
    QDoubleSpinBox* archThickness_ = nullptr;
    QCheckBox* smooth_ = nullptr;
    QComboBox* turn_ = nullptr;
    QLabel* shapeHint_ = nullptr;
    QWidget* paramRows_[8] = {};
    QPushButton* decalButton_ = nullptr;
    QDoubleSpinBox *decalWidth_ = nullptr, *decalHeight_ = nullptr, *decalDepth_ = nullptr;
    std::vector<std::string> prefabFiles_;
    std::unique_ptr<tools::PrefabPreviews> previews_;
    std::map<std::string, QIcon> icons_;          // a prefab with a null icon could not be read
    std::map<std::string, QString> iconTips_;     // its size, or why it has no picture
    std::deque<std::string> iconQueue_;
    QTimer iconTimer_;
    std::vector<std::string> entityClasses_, lightClasses_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> keyvalues_;   // extra keyvalues a starter entity is added with
    bool syncing_ = false;
};

} // namespace native::ui
