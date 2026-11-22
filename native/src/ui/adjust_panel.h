// ui/adjust_panel -- the Adjust dock (10_UI_AND_INPUT.md U1, Blender's "adjust last operation"): the parameters of the last operation (Make hollow: the wall thickness) stay
// editable while nothing else was done; changing one undoes the operation and runs it again, so the history keeps one step. It shows a hint when there is nothing to adjust.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U1), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QTimer;
class QVBoxLayout;

namespace native::ui {

class MainWindow;

class AdjustPanel : public QWidget {
    Q_OBJECT
public:
    explicit AdjustPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Rebuilds the fields from the window's last operation now.
    void refreshNow();
    /// The title shown ("" when there is nothing to adjust) and the field of a parameter (by its key; null if none).
    QString title() const;
    QDoubleSpinBox* field(const QString& key) const;
    /// Applies the values of the fields (a change of a field does it after a short pause); false when there is nothing to adjust or the values were refused.
    bool applyNow();

private:
    MainWindow& win_;
    QVBoxLayout* fields_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QTimer* timer_ = nullptr;      // refresh
    QTimer* applyTimer_ = nullptr; // the pause after a field changed
    std::vector<QDoubleSpinBox*> spins_;
    bool filling_ = false;
};

} // namespace native::ui
