// ui/tool_options_panel -- see tool_options_panel.h.
#include "ui/tool_options_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "render/viewport_window.h"
#include "tools/shapes.h"
#include "tools/status_text.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

struct ShapeEntry {
    tools::ShapeId id;
    const char* label;
};
const ShapeEntry kShapes[] = {{tools::ShapeId::Box, "Box"},   {tools::ShapeId::Cylinder, "Cylinder"}, {tools::ShapeId::Wedge, "Wedge"},
                              {tools::ShapeId::Arch, "Arch"}, {tools::ShapeId::Stairs, "Stairs"},     {tools::ShapeId::Sphere, "Sphere"}};

QString num(double v) { return QString::fromStdString(tools::shortNumber(v)); }

void fillValues(QComboBox* c, const std::vector<double>& values, const QString& unit) {
    for (const double v : values) c->addItem(num(v) + unit, v);
}

void selectValue(QComboBox* c, double value) {
    for (int i = 0; i < c->count(); ++i)
        if (std::abs(c->itemData(i).toDouble() - value) < 1e-9) {
            c->setCurrentIndex(i);
            return;
        }
    c->setCurrentIndex(-1);
}

QWidget* labelled(QVBoxLayout* into, const QString& label, QWidget* w) {
    auto* holder = new QWidget;
    auto* h = new QHBoxLayout(holder);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel(label), 0);
    h->addWidget(w, 1);
    into->addWidget(holder);
    return holder;
}

} // namespace

ToolOptionsPanel::ToolOptionsPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("ToolOptionsPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("toolTitle"));
    QFont f = title_->font();
    f.setBold(true);
    title_->setFont(f);
    v->addWidget(title_);

    // snapping: of the active viewport
    auto* snapTitle = new QLabel(QStringLiteral("Snapping"));
    snapTitle->setContentsMargins(0, 6, 0, 0);
    v->addWidget(snapTitle);
    snap_ = new QCheckBox(QStringLiteral("Snap to grid"));
    snap_->setObjectName(QStringLiteral("snap"));
    v->addWidget(snap_);
    grid_ = new QComboBox;
    grid_->setObjectName(QStringLiteral("grid"));
    fillValues(grid_, render::gridSizes(), QStringLiteral(" m"));
    labelled(v, QStringLiteral("Grid"), grid_);
    rotation_ = new QComboBox;
    rotation_->setObjectName(QStringLiteral("rotation"));
    fillValues(rotation_, render::rotSnaps(), QString::fromUtf8("\xC2\xB0"));
    labelled(v, QStringLiteral("Rotation"), rotation_);
    scale_ = new QComboBox;
    scale_->setObjectName(QStringLiteral("scale"));
    fillValues(scale_, render::scaleSnaps(), QString());
    labelled(v, QStringLiteral("Scale"), scale_);
    QObject::connect(snap_, &QCheckBox::clicked, this, [this] {
        if (!syncing_) win_.runAction("snap.toggle");
    });
    const auto bind = [this](QComboBox* c, const char* id) {
        QObject::connect(c, &QComboBox::activated, this, [this, c, id](int i) {
            if (!syncing_ && i >= 0) win_.runAction(id, nlohmann::json{{"value", c->itemData(i).toDouble()}});
        });
    };
    bind(grid_, "snap.grid_size");
    bind(rotation_, "snap.rotation");
    bind(scale_, "snap.scale");

    // the object tool: the gizmo
    selectGroup_ = new QWidget;
    selectGroup_->setObjectName(QStringLiteral("group:select"));
    auto* sv = new QVBoxLayout(selectGroup_);
    sv->setContentsMargins(0, 6, 0, 0);
    sv->addWidget(new QLabel(QStringLiteral("Gizmo")));
    auto* row = new QHBoxLayout;
    auto* group = new QButtonGroup(selectGroup_);
    group->setExclusive(true);
    for (const char* mode : {"move", "rotate", "scale"}) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("gizmo:") + QString::fromLatin1(mode));
        QString label = QString::fromLatin1(mode);
        label[0] = label[0].toUpper();
        b->setText(label);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        group->addButton(b);
        row->addWidget(b);
        gizmo_.push_back(b);
        const std::string m = mode;
        QObject::connect(b, &QToolButton::clicked, this, [this, m] {
            if (!syncing_) win_.runAction("gizmo.mode", nlohmann::json{{"mode", m}});
        });
    }
    sv->addLayout(row);
    v->addWidget(selectGroup_);

    // the box tool: the shape and its numbers
    boxGroup_ = new QWidget;
    boxGroup_->setObjectName(QStringLiteral("group:box"));
    auto* bv = new QVBoxLayout(boxGroup_);
    bv->setContentsMargins(0, 6, 0, 0);
    shape_ = new QComboBox;
    shape_->setObjectName(QStringLiteral("shape"));
    for (const ShapeEntry& s : kShapes) shape_->addItem(QString::fromLatin1(s.label), static_cast<int>(s.id));
    labelled(bv, QStringLiteral("Shape"), shape_);
    QObject::connect(shape_, &QComboBox::activated, this, [this](int i) {
        if (!syncing_ && i >= 0) win_.setShape(static_cast<tools::ShapeId>(shape_->itemData(i).toInt()));
    });
    const auto spin = [&](const char* name, const QString& label, int lo, int hi) {
        auto* s = new QSpinBox;
        s->setObjectName(QStringLiteral("param:") + QString::fromLatin1(name));
        s->setRange(lo, hi);
        s->setKeyboardTracking(false);
        rows_[QString::fromLatin1(name)] = labelled(bv, label, s);
        QObject::connect(s, &QSpinBox::valueChanged, this, [this] { applyShape(); });
        return s;
    };
    sides_ = spin("sides", QStringLiteral("Sides"), 3, 64);
    steps_ = spin("steps", QStringLiteral("Steps"), 2, 64);
    segments_ = spin("segments", QStringLiteral("Segments"), 4, 32);
    rings_ = spin("rings", QStringLiteral("Rings"), 2, 16);
    archSegments_ = spin("archSegments", QStringLiteral("Arch segments"), 3, 64);
    archThickness_ = new QDoubleSpinBox;
    archThickness_->setObjectName(QStringLiteral("param:archThickness"));
    archThickness_->setRange(0.05, 20);
    archThickness_->setSingleStep(0.05);
    archThickness_->setDecimals(2);
    archThickness_->setKeyboardTracking(false);
    rows_[QStringLiteral("archThickness")] = labelled(bv, QStringLiteral("Wall (m)"), archThickness_);
    QObject::connect(archThickness_, &QDoubleSpinBox::valueChanged, this, [this] { applyShape(); });
    smooth_ = new QCheckBox;
    smooth_->setObjectName(QStringLiteral("param:smooth"));
    rows_[QStringLiteral("smooth")] = labelled(bv, QStringLiteral("Smooth"), smooth_);
    QObject::connect(smooth_, &QCheckBox::clicked, this, [this] { applyShape(); });
    turn_ = new QComboBox;
    turn_->setObjectName(QStringLiteral("param:turn"));
    for (int t = 0; t < 4; ++t) turn_->addItem(QString::number(t * 90) + QString::fromUtf8("\xC2\xB0"), t);
    rows_[QStringLiteral("turn")] = labelled(bv, QStringLiteral("Orientation"), turn_);
    QObject::connect(turn_, &QComboBox::activated, this, [this](int) { applyShape(); });
    v->addWidget(boxGroup_);
    v->addStretch(1);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    const auto later = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::toolChanged, this, later);
    QObject::connect(&win_, &MainWindow::viewportsChanged, this, later);
    QObject::connect(&win_, &MainWindow::selectionChanged, this, later);
    later();   // the window is still being built (no selection yet): the fields are filled when it is up
}

QString ToolOptionsPanel::title() const { return title_->text(); }

QWidget* ToolOptionsPanel::part(const QString& name) const { return findChild<QWidget*>(name); }

void ToolOptionsPanel::applyShape() {
    if (syncing_) return;
    tools::ShapeParams p = win_.shapeParams();
    p.sides = sides_->value();
    p.steps = steps_->value();
    p.segments = segments_->value();
    p.rings = rings_->value();
    p.smooth = smooth_->isChecked();
    p.archSegments = archSegments_->value();
    p.archThickness = archThickness_->value();
    p.turn = turn_->currentData().toInt();
    win_.setShapeParams(p);
}

void ToolOptionsPanel::refreshNow() {
    timer_->stop();
    syncing_ = true;
    const bool box = win_.tool() == MainWindow::Tool::Box;
    const tools::ShapeId shape = win_.shape();
    QString title = box ? QStringLiteral("Tool: Box") : QStringLiteral("Tool: Select");
    if (box)
        for (const ShapeEntry& s : kShapes)
            if (s.id == shape) title += QStringLiteral(" (") + QString::fromLatin1(s.label).toLower() + QStringLiteral(")");
    if (!box && win_.meshMode()) title += QStringLiteral(", mesh mode");
    title_->setText(title);
    selectGroup_->setVisible(!box);
    boxGroup_->setVisible(box);

    const render::ViewportWindow* vp = win_.activeViewport();
    const bool have = vp != nullptr;
    for (QWidget* w : {static_cast<QWidget*>(snap_), static_cast<QWidget*>(grid_), static_cast<QWidget*>(rotation_), static_cast<QWidget*>(scale_), static_cast<QWidget*>(selectGroup_)})
        w->setEnabled(have);
    if (have) {
        const render::ViewportSettings s = vp->rig().settings();
        snap_->setChecked(s.snapEnabled);
        selectValue(grid_, s.gridSize);
        selectValue(rotation_, s.rotSnap);
        selectValue(scale_, s.scaleSnap);
        const int g = s.gizmo == render::GizmoMode::Rotate ? 1 : s.gizmo == render::GizmoMode::Scale ? 2 : 0;
        for (size_t i = 0; i < gizmo_.size(); ++i) gizmo_[i]->setChecked(static_cast<int>(i) == g);
    }
    if (box) {
        const tools::ShapeParams& p = win_.shapeParams();
        for (int i = 0; i < shape_->count(); ++i)
            if (shape_->itemData(i).toInt() == static_cast<int>(shape)) shape_->setCurrentIndex(i);
        sides_->setValue(p.sides);
        steps_->setValue(p.steps);
        segments_->setValue(p.segments);
        rings_->setValue(p.rings);
        smooth_->setChecked(p.smooth);
        archSegments_->setValue(p.archSegments);
        archThickness_->setValue(p.archThickness);
        turn_->setCurrentIndex(std::clamp(p.turn, 0, 3));
        using S = tools::ShapeId;
        rows_["sides"]->setVisible(shape == S::Cylinder);
        rows_["steps"]->setVisible(shape == S::Stairs);
        rows_["segments"]->setVisible(shape == S::Sphere);
        rows_["rings"]->setVisible(shape == S::Sphere);
        rows_["smooth"]->setVisible(shape == S::Cylinder || shape == S::Sphere);
        rows_["archSegments"]->setVisible(shape == S::Arch);
        rows_["archThickness"]->setVisible(shape == S::Arch);
        rows_["turn"]->setVisible(shape != S::Box);
    }
    syncing_ = false;
}

} // namespace native::ui
