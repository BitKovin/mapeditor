// ui/texture_props_window -- the Texture Properties window (REQUIREMENTS section 19 A1-A6, shell/src/ui/texturePropsWindow.ts): one texture's pixels per metre, how it repeats on each
// axis (repeat / mirror / anchor with the split, fill and an alternate texture), the edge handling (auto / wrap / clamp), transparency and the hotspots (drawn, moved and removed on
// the picture of the texture). The value being edited is the sidecar JSON (tools/texturing/props_edit); every change is written to Core a moment after the last one (and when the
// window closes) and the scene is rebuilt with the new properties. A non-modal tool window; the wall preview of the Shell and the Relief editor (T11.5) are not part of it.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (texturePropsWindow), 08_MILESTONES.md M11 item 4.
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "tools/texturing/hotspots.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;

namespace native::ui {

class MainWindow;

// The picture of the texture with its hotspot rectangles: drag on empty space draws one, drag a rectangle moves it, click selects (image fractions, y down).
class HotspotCanvas : public QWidget {
    Q_OBJECT
public:
    explicit HotspotCanvas(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {300, 300}; }
    void setImage(const QImage& image) { image_ = image; update(); }
    void setHotspots(const std::vector<native::tools::tex::Hotspot>& h, int selected);
    const std::vector<native::tools::tex::Hotspot>& hotspots() const { return hotspots_; }
    int selected() const { return selected_; }
    void select(int index);
    // Pointer in canvas pixels (the mouse events call these; tests too).
    void press(const QPointF& p);
    void move(const QPointF& p);
    void release();
    QPointF toImage(const QPointF& p) const;   // canvas pixels -> image fractions
    QPointF toPixel(const QPointF& f) const;
    QRectF imageRect() const;

signals:
    void changed(const std::vector<native::tools::tex::Hotspot>& hotspots, int selected);   // while dragging and after
    void finished();                                                                          // a drag ended

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QImage image_;
    std::vector<native::tools::tex::Hotspot> hotspots_;
    int selected_ = -1;
    enum class Mode { None, Draw, Move } mode_ = Mode::None;
    QPointF start_;
    native::tools::tex::Hotspot startRect_;
    int drawIndex_ = -1;
};

class TexturePropsDialog : public QDialog {
    Q_OBJECT
public:
    TexturePropsDialog(MainWindow& window, const std::string& texture);
    ~TexturePropsDialog() override;

    const std::string& texture() const { return texture_; }
    // The value being edited, and what was last written.
    const nlohmann::json& props() const { return draft_; }
    // Writes now when there are unsaved changes; returns "" on success (or nothing to write), else Core's reason.
    std::string flush();
    bool dirty() const { return dirty_; }
    // The edited sidecar becomes `props` (as if every field had been typed); used by "Revert" and tests.
    void setProps(const nlohmann::json& props);
    // A named child widget (see the objectName list in the .cpp): "ppm", "modeU", "anchorV", "splitU", "fillU", "edgeU", "altU", "altOffU0", "transMode", "cutoff", "opacity",
    // "hotspots", "hx", "hy", "hw", "hh", "canvas".
    QWidget* part(const QString& name) const;
    HotspotCanvas* canvas() const { return canvas_; }
    QString status() const;

private:
    void build();
    void fill();            // the widgets from draft_
    void edited();          // draft_ changed: apply, schedule the write
    void axisEdited(int axis);
    void hotspotsChanged(const std::vector<native::tools::tex::Hotspot>& h, int selected);

    MainWindow& win_;
    std::string texture_;
    nlohmann::json draft_, original_;
    bool dirty_ = false, filling_ = false;
    QTimer* saveTimer_ = nullptr;
    QLabel* status_ = nullptr;
    QDoubleSpinBox* ppm_ = nullptr;
    QComboBox *mode_[2] = {}, *anchor_[2] = {}, *fill_[2] = {}, *edge_[2] = {};
    QDoubleSpinBox *split_[2] = {}, *altOff_[2][2] = {};
    QLineEdit* alt_[2] = {};
    QComboBox* transMode_ = nullptr;
    QDoubleSpinBox *cutoff_ = nullptr, *opacity_ = nullptr;
    QListWidget* list_ = nullptr;
    QDoubleSpinBox* rect_[4] = {};
    HotspotCanvas* canvas_ = nullptr;
    QLabel* info_ = nullptr;
};

} // namespace native::ui
