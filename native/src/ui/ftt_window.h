// ui/ftt_window -- the Fast Texture Tool window (shell/src/ui/fastTextureTool.ts; REQUIREMENTS section 6 B5, Scythe's Shift+Q): a floating, non-modal window that shows the UV island
// of the selected faces of one brush over the texture and edits it by direct manipulation: drag the body = move, the corner / edge handles = scale (Shift on a corner keeps
// the aspect), the round handle above = rotate (Shift = 15 degree steps); Mirror U / V, Rot -90 / +90, Align edge, Follow Quads / Ortho / Axial, snap, wheel zoom, middle drag pan.
// The dashed rectangle of a face is the shape it would have at the texture's own pixels per metre. The island follows the selection. All maths is tools/texturing/ftt; the
// pointer handlers take window pixels so tests can drive them without events. (The Shell's Lightmap mode needs the baked pages and is part of the lightmap view task.)
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (fastTextureTool), 08_MILESTONES.md M11 item 3.
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tools/texturing/ftt.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace native::ui {

class MainWindow;
struct FttTarget;

class FttCanvas : public QWidget {
    Q_OBJECT
public:
    explicit FttCanvas(MainWindow& window, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {600, 420}; }

    // Re-reads the selection (`fit` also frames the island).
    void refresh(bool fit = false);
    void frame();
    // What is shown.
    size_t faceCount() const { return polys_.size(); }
    std::optional<tools::tex::UvBounds> bounds() const { return bounds_; }
    QString description() const { return info_; }
    // View: pixels per repeat and the pixel of UV (0, 0).
    double scale() const { return scale_; }
    QPointF uvToPixel(const tools::tex::V2& uv) const { return {ox_ + uv[0] * scale_, oy_ + uv[1] * scale_}; }
    tools::tex::V2 pixelToUv(const QPointF& p) const { return {(p.x() - ox_) / scale_, (p.y() - oy_) / scale_}; }
    // Pointer (pixels of the canvas): press picks a handle / the body (or an edge in Align mode), move drags, release ends. `shift` = keep the aspect / 15 degree steps.
    bool press(const QPointF& p);
    void move(const QPointF& p, bool shift);
    void release();
    void cancelDrag();
    bool dragging() const { return drag_.has_value(); }
    tools::tex::Handle handleAt(const QPointF& p) const;
    void zoomAt(const QPointF& p, double factor);
    void pan(const QPointF& delta);
    void setSnap(double repeats) { snap_ = repeats; }
    void setAlign(bool on);
    bool align() const { return align_; }
    const QString& status() const { return status_; }
    void setStatus(const QString& s) { status_ = s; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

signals:
    void infoChanged();

private:
    const QImage* textureImage(const std::string& path);
    std::vector<std::vector<tools::tex::V2>> polygons() const;

    MainWindow& win_;
    std::vector<tools::tex::IslandFace> polys_;
    std::optional<tools::tex::UvBounds> bounds_;
    double scale_ = 120, ox_ = 40, oy_ = 40;
    double snap_ = 0;
    bool align_ = false;
    std::optional<std::array<tools::tex::V2, 2>> hoverEdge_;
    struct Drag {
        tools::tex::Handle kind;
        tools::tex::V2 start;
        tools::tex::UvBounds bounds;
    };
    std::optional<Drag> drag_;
    std::optional<QPointF> panFrom_;
    QString info_, status_;
    std::map<std::string, QImage> images_;
};

class FttWindow : public QDialog {
    Q_OBJECT
public:
    explicit FttWindow(MainWindow& window);
    FttCanvas* canvas() const { return canvas_; }
    // Re-reads the selection now (the window also follows the document and the selection by itself).
    void refreshNow(bool fit = false);
    // The toolbar buttons by name: "mirrorU", "mirrorV", "rotM90", "rotP90", "align", "follow", "ortho", "axial", "frame".
    QPushButton* button(const QString& name) const;

private:
    MainWindow& win_;
    FttCanvas* canvas_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace native::ui
