// ui/surface_panel -- the Surface dock (the Surface Inspector of shell/src/ui/texturePanel.ts): the texture of the selected face (else the faces of the selected brushes), its shift
// (texels), stretch (multiple of the natural size), rotation and shear, Mirror / Reset / Fit / Hotspot / Match Grid, the justify, project and repeat buttons, the lightmap density
// and the texel density readout, plus Copy UV / Paste UV / Flow / Lift. The values come from tools/texturing/inspector; every edit is a MainWindow::textureCommand (one undo step).
// A field that has the keyboard is not overwritten when the document changes (typing must not lose the caret).
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (texturePanel), 08_MILESTONES.md M11 item 3.
#pragma once

#include <QWidget>

#include "tools/texturing/inspector.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace native::ui {

class MainWindow;

class SurfacePanel : public QWidget {
    Q_OBJECT
public:
    explicit SurfacePanel(MainWindow& window, QWidget* parent = nullptr);

    // The values shown now (after the last refresh).
    const tools::tex::SurfaceFields& fields() const { return fields_; }
    void refreshNow();
    // A named child widget (objectName: "texture", "shiftU", "shiftV", "stretchU", "stretchV", "rotate", "shear", "lightmap", "modeU", "modeV", "edgeU", "edgeV", "density",
    // and the buttons "btn:<op>" / "btn:<op>:<arg>"); null when there is none.
    QWidget* part(const QString& name) const;
    // Makes the edit of a field as if the user typed it (the widgets call this; tests too): `field` is one of the names above.
    bool setField(const QString& field, double value);
    bool setTexture(const QString& texture);

private:
    void build();
    void fill();
    QPushButton* button(const QString& text, const QString& name, const std::string& op, const nlohmann::json& params = nlohmann::json::object(), const QString& tip = {});
    QDoubleSpinBox* spin(const QString& name, double lo, double hi, int decimals, double step);
    void setRepeat(int axis, bool edge, const QString& value);

    MainWindow& win_;
    QTimer* timer_ = nullptr;
    tools::tex::SurfaceFields fields_;
    QLabel* title_ = nullptr;
    QLineEdit* texture_ = nullptr;
    QLabel* density_ = nullptr;
    QDoubleSpinBox *shift_[2] = {}, *stretch_[2] = {}, *rotate_ = nullptr, *shear_ = nullptr, *lightmap_ = nullptr;
    QComboBox *mode_[2] = {}, *edge_[2] = {};
    bool filling_ = false;
};

} // namespace native::ui
