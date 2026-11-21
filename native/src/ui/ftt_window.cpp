// ui/ftt_window -- see ftt_window.h.
#include "ui/ftt_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "core/albedo.h"
#include "core/textures.h"
#include "tools/texturing/face_edit.h"
#include "ui/main_window.h"

namespace native::ui {

namespace tex = tools::tex;

namespace {

constexpr double kMinScale = 8, kMaxScale = 4000;
const double kSnaps[] = {0, 1, 0.5, 0.25, 0.125, 0.0625};
const char* kSnapLabels[] = {"Snap: off", "Snap: 1 repeat", "Snap: 1/2", "Snap: 1/4", "Snap: 1/8", "Snap: 1/16"};

QString fmt(double v) { return QString::number(v, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$"))); }

} // namespace

// ---- the canvas -----------------------------------------------------------------------------------------------------------------------

FttCanvas::FttCanvas(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("fttCanvas"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(300, 220);
}

std::vector<std::vector<tex::V2>> FttCanvas::polygons() const {
    std::vector<std::vector<tex::V2>> out;
    for (const tex::IslandFace& f : polys_) out.push_back(f.poly);
    return out;
}

void FttCanvas::frame() {
    if (!bounds_) return;
    const double w = std::max(bounds_->max[0] - bounds_->min[0], 0.25), h = std::max(bounds_->max[1] - bounds_->min[1], 0.25);
    scale_ = std::clamp(std::min((width() - 110.0) / w, (height() - 110.0) / h), kMinScale, kMaxScale);
    const tex::V2 c = tex::boundsCentre(*bounds_);
    ox_ = width() / 2.0 - c[0] * scale_;
    oy_ = height() / 2.0 - c[1] * scale_;
    update();
}

void FttCanvas::refresh(bool fit) {
    const auto target = win_.fttTarget();
    const core::Brush* brush = target ? win_.fttBrush(*target) : nullptr;
    polys_.clear();
    if (brush && target) polys_ = tex::islandPolygons(*brush, target->faceIds, win_.textureInfo());
    bounds_ = tex::polygonsBounds(polygons());
    if (bounds_ && !drag_) {
        const QPointF a = uvToPixel(bounds_->min), b = uvToPixel(bounds_->max);
        const bool onScreen = b.x() > 0 && b.y() > 0 && a.x() < width() && a.y() < height();
        if (fit || !onScreen) frame();
    }
    if (!brush || !target || !bounds_) {
        info_ = QStringLiteral("Select faces (or a brush) in the viewport: their UV island appears here.");
    } else {
        const core::BrushFace* f0 = nullptr;
        for (const core::BrushFace& f : brush->faces)
            if (f.id == target->faceIds.front()) f0 = &f;
        const double bw = bounds_->max[0] - bounds_->min[0], bh = bounds_->max[1] - bounds_->min[1];
        info_ = QString::number(polys_.size()) + (polys_.size() == 1 ? QStringLiteral(" face") : QStringLiteral(" faces")) + QStringLiteral(" of ") + QString::fromStdString(target->brushId) +
                QStringLiteral(" · island ") + fmt(bw) + QStringLiteral(" x ") + fmt(bh) + QStringLiteral(" repeats");
        if (f0) {
            const tex::V2 ideal = tex::idealSize(*brush, *f0, win_.textureInfo());
            info_ += QStringLiteral(" · first face ideal ") + fmt(ideal[0]) + QStringLiteral(" x ") + fmt(ideal[1]) + QStringLiteral(" repeats (dashed)");
        }
    }
    emit infoChanged();
    update();
}

const QImage* FttCanvas::textureImage(const std::string& path) {
    if (const auto it = images_.find(path); it != images_.end()) return it->second.isNull() ? nullptr : &it->second;
    QImage img;
    const auto profile = win_.gameProfile();
    if (profile && !win_.assetsRoot().empty()) {
        if (const auto file = core::readTexture(win_.assetsRoot(), *profile, path))
            if (const auto d = core::decodeImage(file->bytes, file->ext); d && d->width > 0 && d->height > 0 && d->rgba.size() >= static_cast<size_t>(d->width) * d->height * 4)
                img = QImage(d->rgba.data(), d->width, d->height, d->width * 4, QImage::Format_RGBA8888).copy();
    }
    return images_.emplace(path, std::move(img)).first->second.isNull() ? nullptr : &images_[path];
}

void FttCanvas::setAlign(bool on) {
    align_ = on;
    hoverEdge_.reset();
    status_ = on ? QStringLiteral("Click an edge of the island") : QString();
    update();
}

// ---- pointer --------------------------------------------------------------------------------------------------------------------------

tex::Handle FttCanvas::handleAt(const QPointF& p) const {
    if (!bounds_) return tex::Handle::None;
    return tex::hitTest(*bounds_, polygons(), pixelToUv(p), 8 / scale_, 24 / scale_);
}

bool FttCanvas::press(const QPointF& p) {
    const auto target = win_.fttTarget();
    if (!bounds_ || !target) return false;
    const tex::V2 uv = pixelToUv(p);
    if (align_) {
        const auto edge = tex::nearestEdge(polygons(), uv, 10 / scale_);
        const auto t = edge ? tex::alignEdgeXform((*edge)[0], (*edge)[1]) : std::nullopt;
        if (t && *t != tex::kIdentity) {
            win_.fttApply("Align UV edge", *t);
            status_ = QStringLiteral("Edge aligned");
        } else {
            status_ = edge ? QStringLiteral("That edge is already straight") : QStringLiteral("Click near a UV edge");
        }
        return true;
    }
    const tex::Handle hit = handleAt(p);
    if (hit == tex::Handle::None || !win_.fttBeginDrag("Fast texture")) return false;
    drag_ = Drag{hit, uv, *bounds_};
    return true;
}

void FttCanvas::move(const QPointF& p, bool shift) {
    const tex::V2 uv = pixelToUv(p);
    if (drag_) {
        if (const auto target = win_.fttTarget()) win_.fttDragTo(*target, tex::dragXform(drag_->kind, drag_->start, uv, drag_->bounds, {snap_, shift}));
        return;
    }
    if (align_) {
        hoverEdge_ = tex::nearestEdge(polygons(), uv, 10 / scale_);
        setCursor(hoverEdge_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
        return;
    }
    switch (handleAt(p)) {
        case tex::Handle::NW: case tex::Handle::SE: setCursor(Qt::SizeFDiagCursor); break;
        case tex::Handle::NE: case tex::Handle::SW: setCursor(Qt::SizeBDiagCursor); break;
        case tex::Handle::N: case tex::Handle::S: setCursor(Qt::SizeVerCursor); break;
        case tex::Handle::E: case tex::Handle::W: setCursor(Qt::SizeHorCursor); break;
        case tex::Handle::Rotate: setCursor(Qt::OpenHandCursor); break;
        case tex::Handle::Move: setCursor(Qt::SizeAllCursor); break;
        default: setCursor(Qt::ArrowCursor); break;
    }
}

void FttCanvas::release() {
    if (drag_) {
        win_.fttEndDrag(true);
        drag_.reset();
    }
    panFrom_.reset();
}

void FttCanvas::cancelDrag() {
    if (!drag_) return;
    win_.fttEndDrag(false);
    drag_.reset();
}

void FttCanvas::zoomAt(const QPointF& p, double factor) {
    const tex::V2 uv = pixelToUv(p);
    scale_ = std::clamp(scale_ * factor, kMinScale, kMaxScale);
    ox_ = p.x() - uv[0] * scale_;
    oy_ = p.y() - uv[1] * scale_;
    update();
}

void FttCanvas::pan(const QPointF& delta) {
    ox_ += delta.x();
    oy_ += delta.y();
    update();
}

void FttCanvas::mousePressEvent(QMouseEvent* e) {
    setFocus();
    if (e->button() == Qt::MiddleButton) {
        panFrom_ = e->position();
        return;
    }
    if (e->button() == Qt::LeftButton) press(e->position());
}

void FttCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (panFrom_) {
        pan(e->position() - *panFrom_);
        panFrom_ = e->position();
        return;
    }
    move(e->position(), e->modifiers().testFlag(Qt::ShiftModifier));
}

void FttCanvas::mouseReleaseEvent(QMouseEvent*) { release(); }

void FttCanvas::wheelEvent(QWheelEvent* e) { zoomAt(e->position(), std::exp(e->angleDelta().y() * 0.0015)); }

void FttCanvas::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && drag_) cancelDrag();
    else QWidget::keyPressEvent(e);
}

// ---- drawing --------------------------------------------------------------------------------------------------------------------------

void FttCanvas::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.fillRect(rect(), QColor(0x1b, 0x1b, 0x1b));
    const auto target = win_.fttTarget();
    const core::Brush* brush = target ? win_.fttBrush(*target) : nullptr;
    // the texture, repeated (one repeat = scale_ pixels)
    if (brush && target) {
        std::string path = "dev/grey";
        for (const core::BrushFace& f : brush->faces)
            if (f.id == target->faceIds.front() && !f.texture.empty()) path = f.texture;
        if (const QImage* img = textureImage(path)) {
            QBrush pattern{QPixmap::fromImage(*img)};
            pattern.setTransform(QTransform().translate(ox_, oy_).scale(scale_ / img->width(), scale_ / img->height()));
            g.setOpacity(0.55);
            g.fillRect(rect(), pattern);
            g.setOpacity(1);
        }
    }
    // the repeat grid
    g.setPen(QColor(255, 255, 255, 46));
    const int u0 = static_cast<int>(std::floor(-ox_ / scale_)), u1 = static_cast<int>(std::ceil((width() - ox_) / scale_));
    const int v0 = static_cast<int>(std::floor(-oy_ / scale_)), v1 = static_cast<int>(std::ceil((height() - oy_) / scale_));
    if (u1 - u0 < 200 && v1 - v0 < 200) {
        for (int u = u0; u <= u1; ++u) g.drawLine(QPointF(ox_ + u * scale_, 0), QPointF(ox_ + u * scale_, height()));
        for (int v = v0; v <= v1; ++v) g.drawLine(QPointF(0, oy_ + v * scale_), QPointF(width(), oy_ + v * scale_));
    }
    if (!bounds_ || !brush) {
        g.setPen(QColor(0x88, 0x88, 0x88));
        g.drawText(rect(), Qt::AlignCenter, QStringLiteral("Nothing selected"));
        return;
    }
    // the faces and their ideal shapes
    for (const tex::IslandFace& f : polys_) {
        QPolygonF poly;
        for (const tex::V2& uv : f.poly) poly << uvToPixel(uv);
        g.setBrush(QColor(10, 132, 232, 56));
        g.setPen(QPen(QColor(0x6c, 0xb6, 0xff), 1.5));
        g.drawPolygon(poly);
        for (const core::BrushFace& bf : brush->faces) {
            if (bf.id != f.id) continue;
            const auto b = tex::polygonsBounds({f.poly});
            const tex::V2 ideal = tex::idealSize(*brush, bf, win_.textureInfo());
            if (!b) break;
            const QPointF o = uvToPixel(b->min);
            g.setBrush(Qt::NoBrush);
            g.setPen(QPen(QColor(80, 170, 255, 230), 1, Qt::DashLine));
            g.drawRect(QRectF(o.x(), o.y(), ideal[0] * scale_, ideal[1] * scale_));
        }
    }
    if (align_ && hoverEdge_) {
        g.setPen(QPen(QColor(0xff, 0xd3, 0x4d), 3));
        g.drawLine(uvToPixel((*hoverEdge_)[0]), uvToPixel((*hoverEdge_)[1]));
    }
    // the island box and the handles
    g.setBrush(Qt::NoBrush);
    g.setPen(QPen(QColor(0xff, 0xd3, 0x4d), 1));
    g.drawRect(QRectF(uvToPixel(bounds_->min), uvToPixel(bounds_->max)));
    const double gap = 24 / scale_;
    const QPointF rp = uvToPixel(tex::handlePoint(*bounds_, tex::Handle::Rotate, gap)), np = uvToPixel(tex::handlePoint(*bounds_, tex::Handle::N));
    g.drawLine(np, rp);
    g.setBrush(QColor(0xff, 0xd3, 0x4d));
    g.drawEllipse(rp, 5, 5);
    g.setBrush(Qt::white);
    for (const tex::Handle h : tex::kScaleHandles) {
        const QPointF p = uvToPixel(tex::handlePoint(*bounds_, h));
        g.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8));
    }
}

// ---- the window -----------------------------------------------------------------------------------------------------------------------

FttWindow::FttWindow(MainWindow& window) : QDialog(&window, Qt::Tool), win_(window) {
    setObjectName(QStringLiteral("fttWindow"));
    setWindowTitle(QStringLiteral("Fast Texture Tool"));
    setModal(false);
    auto* v = new QVBoxLayout(this);
    auto* bar = new QHBoxLayout;
    const auto addButton = [&](const QString& text, const QString& name, const QString& tip, const std::function<void()>& run) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("btn:") + name);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        QObject::connect(b, &QPushButton::clicked, this, [run] { run(); });
        bar->addWidget(b);
        return b;
    };
    const auto centred = [this](const std::string& label, const std::function<tex::Xform(const tex::V2&)>& make) {
        return [this, label, make] {
            const auto b = canvas_->bounds();
            if (!b || !win_.fttTarget()) {
                canvas_->setStatus(QStringLiteral("Select faces first"));
                status_->setText(canvas_->status());
                return;
            }
            canvas_->setStatus({});
            win_.fttApply(label, make(tex::boundsCentre(*b)));
        };
    };
    canvas_ = new FttCanvas(win_, this);
    addButton(QStringLiteral("Mirror U"), QStringLiteral("mirrorU"), QStringLiteral("Flip the island left-right about its centre"), centred("Mirror UV left-right", [](const tex::V2& c) { return tex::mirrorXformAbout('u', c[0]); }));
    addButton(QStringLiteral("Mirror V"), QStringLiteral("mirrorV"), QStringLiteral("Flip the island top-bottom about its centre"), centred("Mirror UV top-bottom", [](const tex::V2& c) { return tex::mirrorXformAbout('v', c[1]); }));
    addButton(QStringLiteral("Rot -90"), QStringLiteral("rotM90"), QStringLiteral("Rotate the island 90 degrees counter-clockwise on screen"), centred("Rotate UV -90", [](const tex::V2& c) { return tex::rotateXformAbout(-90, c); }));
    addButton(QStringLiteral("Rot +90"), QStringLiteral("rotP90"), QStringLiteral("Rotate the island 90 degrees clockwise on screen"), centred("Rotate UV 90", [](const tex::V2& c) { return tex::rotateXformAbout(90, c); }));
    auto* alignBtn = addButton(QStringLiteral("Align edge"), QStringLiteral("align"), QStringLiteral("Then click an edge of the island: it is turned until horizontal or vertical"), [this] {
        canvas_->setAlign(!canvas_->align());
        status_->setText(canvas_->status());
    });
    alignBtn->setCheckable(true);
    const auto unwrap = [this](tex::UnwrapMode mode) {
        return [this, mode] {
            if (!win_.fttTarget()) {
                status_->setText(QStringLiteral("Select faces first"));
                return;
            }
            const size_t n = win_.fttUnwrap(mode);
            status_->setText(n ? QString() : mode == tex::UnwrapMode::Follow ? QStringLiteral("Follow Quads needs two or more faces (the first one is the source)") : QStringLiteral("Nothing changed"));
        };
    };
    addButton(QStringLiteral("Follow Quads"), QStringLiteral("follow"), QStringLiteral("Continue the first selected face's UVs over the others along their shared edges (Flow)"), unwrap(tex::UnwrapMode::Follow));
    addButton(QStringLiteral("Ortho"), QStringLiteral("ortho"), QStringLiteral("Project every face on its own, readable from the front"), unwrap(tex::UnwrapMode::Ortho));
    addButton(QStringLiteral("Axial"), QStringLiteral("axial"), QStringLiteral("Project every face along the world axis it faces"), unwrap(tex::UnwrapMode::Axial));
    auto* snap = new QComboBox(this);
    snap->setObjectName(QStringLiteral("snap"));
    snap->setToolTip(QStringLiteral("Snap of move and scale, in texture repeats"));
    for (const char* l : kSnapLabels) snap->addItem(QString::fromLatin1(l));
    QObject::connect(snap, &QComboBox::activated, this, [this](int i) { canvas_->setSnap(kSnaps[std::clamp(i, 0, 5)]); });
    bar->addWidget(snap);
    addButton(QStringLiteral("Frame"), QStringLiteral("frame"), QStringLiteral("Zoom to the island"), [this] { canvas_->frame(); });
    bar->addStretch(1);
    v->addLayout(bar);
    v->addWidget(canvas_, 1);
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("info"));
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(info_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    v->addWidget(status_);
    auto* hint = new QLabel(QStringLiteral("Drag: move · handles: scale (Shift on a corner = aspect, round handle = rotate, Shift = 15°) · wheel: zoom · middle drag: pan"), this);
    hint->setEnabled(false);
    hint->setWordWrap(true);
    v->addWidget(hint);
    resize(640, 560);

    QObject::connect(this, &QDialog::finished, this, [this](int) { canvas_->cancelDrag(); });
    QObject::connect(canvas_, &FttCanvas::infoChanged, this, [this] { info_->setText(canvas_->description()); });
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { if (isVisible()) canvas_->refresh(false); });
    const auto schedule = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::documentEdited, this, schedule);
    QObject::connect(&win_, &MainWindow::documentChanged, this, schedule);
    QObject::connect(&win_, &MainWindow::selectionChanged, this, schedule);
}

QPushButton* FttWindow::button(const QString& name) const { return findChild<QPushButton*>(QStringLiteral("btn:") + name); }

void FttWindow::refreshNow(bool fit) {
    timer_->stop();
    canvas_->refresh(fit);
}

} // namespace native::ui
