// ui/tool_options_panel -- the Tool dock (10_UI_AND_INPUT.md U2): the settings of the active tool in one place instead of separate tabs: the snapping of the active viewport
// (snap on / off, grid, rotation, scale), the transform gizmo with the object tool, the shape and its numbers with the box tool. It only shows and sets what the window
// already has (the viewport settings through the snap / gizmo actions, MainWindow::setShape / setShapeParams), so the Place panel and this one agree.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U2), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QString>
#include <QWidget>

#include <map>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;

namespace native::ui {

class MainWindow;

class ToolOptionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolOptionsPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Shows the window's state in the fields now (they follow by themselves, a turn of the event loop after a change).
    void refreshNow();
    /// "Tool: Select" / "Tool: Box (cylinder)".
    QString title() const;
    /// A field or group by object name ("snap", "grid", "rotation", "scale", "gizmo:move|rotate|scale", "shape", "param:<name>", "group:select", "group:box").
    QWidget* part(const QString& name) const;

private:
    void applyShape();

    MainWindow& win_;
    QLabel* title_ = nullptr;
    QCheckBox* snap_ = nullptr;
    QComboBox *grid_ = nullptr, *rotation_ = nullptr, *scale_ = nullptr, *shape_ = nullptr, *turn_ = nullptr;
    QWidget *selectGroup_ = nullptr, *boxGroup_ = nullptr;
    std::vector<QToolButton*> gizmo_;
    QSpinBox *sides_ = nullptr, *steps_ = nullptr, *segments_ = nullptr, *rings_ = nullptr, *archSegments_ = nullptr;
    QDoubleSpinBox* archThickness_ = nullptr;
    QCheckBox* smooth_ = nullptr;
    std::map<QString, QWidget*> rows_;   // the labelled rows of the box group (shown by the shape)
    QTimer* timer_ = nullptr;
    bool syncing_ = false;
};

} // namespace native::ui
