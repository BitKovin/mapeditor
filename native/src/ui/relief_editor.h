// ui/relief_editor -- the Relief editor window (shell/src/ui/reliefEditor.ts + render/reliefView.ts, REQUIREMENTS section 19 A6): one repeat of a texture is a vertical plane (the
// surface the texture lies on) and its relief mesh is edited with the tools of the Mesh editor (select vertices / edges / faces, G R S, extrude, inset, bevel, loop cut, merge,
// dissolve...). Depth is the Z axis: negative sinks into the surface, positive stands out. The mesh is stored with the texture (`relief` in the sidecar). The editing state is
// tools/relief/relief_edit (pure); this file is the view, the input and the side panel.
// The view is drawn with QPainter from the same projection that picks (a software view: the mesh is a few dozen polygons; the textured faces are mapped affinely per triangle). The
// Shell drew with WebGL; a Vulkan viewport for it is not needed for the result. Neighbouring repeats are drawn dimmed.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (reliefEditor, reliefView), 08_MILESTONES.md M11 item 5.
#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <core/texplan.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "render/camera.h"
#include "tools/mesh/pick.h"
#include "tools/modal.h"
#include "tools/relief/relief_edit.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace native::ui {

class MainWindow;
class ReliefEditor;

// The canvas: paints the editor's state and hands the pointer and the keys to it.
class ReliefView : public QWidget {
    Q_OBJECT
public:
    explicit ReliefView(ReliefEditor& editor, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {720, 560}; }
    void setBox(const std::optional<QRectF>& box) { box_ = box; update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    ReliefEditor& ed_;
    std::optional<QRectF> box_;
    QPointF last_, boxStart_;
    Qt::MouseButton drag_ = Qt::NoButton;
};

class ReliefEditor : public QDialog {
    Q_OBJECT
public:
    ReliefEditor(MainWindow& window, const std::string& texture);
    ~ReliefEditor() override;

    const std::string& texture() const { return texture_; }
    native::tools::relief::ReliefSession& session() { return session_; }
    bool dirty() const;
    native::tools::relief::ReliefReport report() const;
    QString status() const { return status_; }
    ReliefView* view() const { return view_; }

    // ---- camera and picking (the view calls these; tests too) ----
    void setView(double yaw, double pitch);
    void frameTile();
    void orbit(double dx, double dy) { cam_.orbit(dx, dy); update3d(); }
    void pan(double dx, double dy);
    void zoom(double wheel) { cam_.zoom(wheel); update3d(); }
    native::render::OrbitCamera& camera() { return cam_; }
    std::optional<native::tools::mesh::Screen> project(const core::Vec3& p);
    native::render::Ray rayAt(double px, double py);
    void click(double px, double py, bool shift);                       // selects the element under the pointer (vertex / edge / face by the mode)
    void boxSelect(double x0, double y0, double x1, double y1, bool shift);
    void hover(double px, double py);                                   // loop cut arming follows the pointer

    // ---- keys and tools ----
    bool key(int qtKey, Qt::KeyboardModifiers mods, const QString& text);   // true = handled
    bool startModal(native::tools::ModalKind kind);
    bool modalActive() const { return modal_.has_value(); }
    void modalMove(double px, double py);
    void modalKey(const std::string& typed);   // a numeric key name ("5", ".", "-", "Backspace")
    void modalAxis(int axis);                  // X / Y / Z (0 / 1 / 2); the same axis again = free
    void modalWheel(double dy);
    void modalConfirm();
    void modalCancel();
    bool loopCutArmed() const { return loopArmed_; }
    void armLoopCut(bool on);
    void loadPreset(native::tools::relief::Preset p);
    void setDepth(double z);
    void removeRelief();
    void undo();
    void redo();
    void setMode(native::tools::mesh::SelMode m);
    bool snapOn() const { return snapOn_; }
    double snapStep() const { return snapStep_; }
    bool showTexture() const { return showTexture_; }
    bool showGhosts() const { return showGhosts_; }
    const QImage& image() const { return image_; }
    void setImage(const QImage& i) { image_ = i; update3d(); }   // the picture the faces are drawn with (tests; the editor loads it itself)
    // The edge ring the armed loop cut would cut (for drawing), empty when none.
    std::vector<std::pair<uint32_t, uint32_t>> loopOverlay() const;
    // The mouse position the view last gave (canvas pixels).
    QPointF mouse() const { return mouse_; }
    void setMouse(const QPointF& p) { mouse_ = p; }

    // ---- saving ----
    std::string save();      // "" = written to the texture's sidecar (a flat mesh removes the relief), else the reason
    bool applyAndClose();    // save, then close
    void discard();          // close without saving
    QWidget* part(const QString& name) const;

    void reject() override;   // asks before dropping unsaved changes (only while the window is visible)

    double modalValue() const { return modal_ ? modal_->value : 0; }
    int modalSegments() const { return modal_ ? modal_->segments : 0; }
    bool inModal(native::tools::ModalKind k) const { return modal_ && modal_->kind == k; }

signals:
    void saved();   // the relief was written to the texture's properties

public slots:
    void refresh();

private:
    void build();
    void changed();
    void run(const std::string& err);
    void syncTexture();
    void update3d();
    std::string say(const std::string& s);
    double step() const { return snapOn_ ? snapStep_ : 0; }
    void rebuildPanel();
    void setHud();
    friend class ReliefView;

    struct Modal {
        native::tools::ModalKind kind = native::tools::ModalKind::Grab;
        int axis = -1;
        std::string typed;
        double value = 0;
        int segments = 1;
        native::render::Vec3 pivot, normal, startPoint, dir, railFrom, railDir;
        native::tools::Pixel screenPivot, mouse0;
        double t0 = 0, railLength = 0, fraction = 0.5, mpp = 0;
        std::vector<uint32_t> faces;
        std::vector<native::tools::mesh::Edge> edges;
        native::tools::mesh::Edge edge;
    };
    bool begin(native::tools::ModalKind kind, std::optional<native::tools::mesh::Edge> edge);
    void endModal(bool confirm);
    void modalApply();
    QSize viewSize() const;
    native::render::Vec3 viewForward();

    MainWindow& win_;
    std::string texture_;
    native::tools::relief::ReliefSession session_;
    std::string startJson_;
    QImage image_;
    bool imageTried_ = false;
    native::render::OrbitCamera cam_;
    bool snapOn_ = true, showTexture_ = true, showGhosts_ = true, loopArmed_ = false;
    double snapStep_ = 0.05;
    std::optional<native::tools::mesh::Edge> loopHover_;
    std::optional<Modal> modal_;
    QString status_;
    QPointF mouse_;
    ReliefView* view_ = nullptr;
    QLabel *hud_ = nullptr, *info_ = nullptr, *state_ = nullptr;
    QWidget* panel_ = nullptr;
    QPushButton *undo_ = nullptr, *redo_ = nullptr;
    QDoubleSpinBox *depth_ = nullptr, *stepBox_ = nullptr;
    QCheckBox *snapCheck_ = nullptr, *texCheck_ = nullptr, *ghostCheck_ = nullptr;
    QWidget* warnings_ = nullptr;
    bool building_ = false, discarding_ = false;
};

} // namespace native::ui
