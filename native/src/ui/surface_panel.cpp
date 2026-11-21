// ui/surface_panel -- see surface_panel.h.
#include "ui/surface_panel.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const char* kModes[] = {"", "repeat", "mirror"};
const char* kModeLabels[] = {"Texture", "Repeat", "Mirror"};
const char* kEdges[] = {"", "wrap", "clamp"};
const char* kEdgeLabels[] = {"Auto", "Wrap", "Clamp"};

} // namespace

SurfacePanel::SurfacePanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("surfacePanel"));
    build();
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { fill(); });
    const auto schedule = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::documentEdited, this, schedule);
    QObject::connect(&win_, &MainWindow::documentChanged, this, schedule);
    QObject::connect(&win_, &MainWindow::selectionChanged, this, schedule);
    QObject::connect(&win_, &MainWindow::entityDefinitionsChanged, this, schedule);
    timer_->start();
}

QDoubleSpinBox* SurfacePanel::spin(const QString& name, double lo, double hi, int decimals, double step) {
    auto* s = new QDoubleSpinBox(this);
    s->setObjectName(name);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setKeyboardTracking(false);   // an edit is committed when it is finished, not on every digit
    QObject::connect(s, &QDoubleSpinBox::valueChanged, this, [this, name](double v) {
        if (!filling_) setField(name, v);
    });
    return s;
}

QPushButton* SurfacePanel::button(const QString& text, const QString& name, const std::string& op, const nlohmann::json& params, const QString& tip) {
    auto* b = new QPushButton(text, this);
    b->setObjectName(QStringLiteral("btn:") + name);
    if (!tip.isEmpty()) b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);   // a click must not take the keyboard from the viewport
    QObject::connect(b, &QPushButton::clicked, this, [this, op, params] { win_.textureCommand(op, params); });
    return b;
}

void SurfacePanel::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* v = new QVBoxLayout(body);

    title_ = new QLabel(QStringLiteral("Nothing selected"), body);
    title_->setObjectName(QStringLiteral("title"));
    title_->setWordWrap(true);
    v->addWidget(title_);

    auto* texRow = new QHBoxLayout;
    texture_ = new QLineEdit(body);
    texture_->setObjectName(QStringLiteral("texture"));
    texture_->setPlaceholderText(QStringLiteral("texture path"));
    texRow->addWidget(texture_, 1);
    auto* apply = new QPushButton(QStringLiteral("Apply"), body);
    apply->setObjectName(QStringLiteral("btn:apply"));
    apply->setFocusPolicy(Qt::NoFocus);
    texRow->addWidget(apply);
    v->addLayout(texRow);
    QObject::connect(texture_, &QLineEdit::returnPressed, this, [this] { setTexture(texture_->text()); });
    QObject::connect(apply, &QPushButton::clicked, this, [this] { setTexture(texture_->text()); });

    auto* grid = new QGridLayout;
    const auto axisRow = [&](int row, const QString& label, QDoubleSpinBox* u, QDoubleSpinBox* w) {
        grid->addWidget(new QLabel(label, body), row, 0);
        grid->addWidget(u, row, 1);
        if (w) grid->addWidget(w, row, 2);
    };
    for (int a = 0; a < 2; ++a) {
        const QString ax = a == 0 ? QStringLiteral("U") : QStringLiteral("V");
        shift_[a] = spin(QStringLiteral("shift") + ax, -1e6, 1e6, 2, 1);
        stretch_[a] = spin(QStringLiteral("stretch") + ax, -1000, 1000, 4, 0.1);
    }
    axisRow(0, QStringLiteral("Shift (texels)"), shift_[0], shift_[1]);
    axisRow(1, QStringLiteral("Stretch (x natural)"), stretch_[0], stretch_[1]);
    rotate_ = spin(QStringLiteral("rotate"), -360, 360, 2, 15);
    shear_ = spin(QStringLiteral("shear"), -10, 10, 3, 0.1);
    axisRow(2, QStringLiteral("Rotate (deg) / Shear"), rotate_, shear_);
    v->addLayout(grid);

    auto* row1 = new QHBoxLayout;
    row1->addWidget(button(QStringLiteral("Mirror U"), QStringLiteral("mirror:0"), "mirror", {{"axis", 0}}));
    row1->addWidget(button(QStringLiteral("Mirror V"), QStringLiteral("mirror:1"), "mirror", {{"axis", 1}}));
    row1->addWidget(button(QStringLiteral("Reset"), QStringLiteral("reset"), "reset", {}, QStringLiteral("Natural size, unrotated, from the bottom-left corner of the face")));
    row1->addWidget(button(QStringLiteral("Fit"), QStringLiteral("fit"), "fit", {}, QStringLiteral("The texture covers the face exactly once")));
    v->addLayout(row1);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(button(QStringLiteral("Hotspot"), QStringLiteral("hotspot"), "hotspot", {}, QStringLiteral("The hotspot of the texture that fits the face best")));
    row2->addWidget(button(QStringLiteral("Match Grid"), QStringLiteral("matchGrid"), "matchGrid"));
    v->addLayout(row2);

    auto* just = new QGroupBox(QStringLiteral("Justify"), body);
    auto* jl = new QHBoxLayout(just);
    for (const char* e : {"top", "bottom", "left", "right", "center"}) {
        QString t = QString::fromLatin1(e);
        t[0] = t[0].toUpper();
        jl->addWidget(button(t, QStringLiteral("justify:") + QString::fromLatin1(e), "justify", {{"edge", e}}));
    }
    v->addWidget(just);

    auto* proj = new QGroupBox(QStringLiteral("Project"), body);
    auto* pl = new QHBoxLayout(proj);
    pl->addWidget(button(QStringLiteral("Axial"), QStringLiteral("axial"), "axial"));
    pl->addWidget(button(QStringLiteral("Ortho"), QStringLiteral("ortho"), "ortho"));
    pl->addWidget(button(QStringLiteral("Cam"), QStringLiteral("cam"), "cam", {}, QStringLiteral("From the active viewport camera")));
    v->addWidget(proj);

    auto* rep = new QGroupBox(QStringLiteral("Repetition of the face"), body);
    auto* rl = new QGridLayout(rep);
    for (int a = 0; a < 2; ++a) {
        const QString ax = a == 0 ? QStringLiteral("U") : QStringLiteral("V");
        mode_[a] = new QComboBox(rep);
        mode_[a]->setObjectName(QStringLiteral("mode") + ax);
        for (const char* l : kModeLabels) mode_[a]->addItem(QString::fromLatin1(l));
        edge_[a] = new QComboBox(rep);
        edge_[a]->setObjectName(QStringLiteral("edge") + ax);
        for (const char* l : kEdgeLabels) edge_[a]->addItem(QString::fromLatin1(l));
        rl->addWidget(new QLabel(ax, rep), a, 0);
        rl->addWidget(mode_[a], a, 1);
        rl->addWidget(edge_[a], a, 2);
        QObject::connect(mode_[a], &QComboBox::activated, this, [this, a](int i) {
            if (!filling_) setRepeat(a, false, QString::fromLatin1(kModes[i]));
        });
        QObject::connect(edge_[a], &QComboBox::activated, this, [this, a](int i) {
            if (!filling_) setRepeat(a, true, QString::fromLatin1(kEdges[i]));
        });
    }
    v->addWidget(rep);

    auto* lm = new QGroupBox(QStringLiteral("Lightmap texel density"), body);
    auto* ll = new QHBoxLayout(lm);
    lightmap_ = spin(QStringLiteral("lightmap"), 0.0625, 16, 4, 0.25);
    ll->addWidget(lightmap_);
    ll->addWidget(button(QStringLiteral("x2"), QStringLiteral("lightmap:x2"), "lightmap", {{"factor", 2.0}}));
    ll->addWidget(button(QStringLiteral("/2"), QStringLiteral("lightmap:half"), "lightmap", {{"factor", 0.5}}));
    ll->addWidget(button(QStringLiteral("Reset"), QStringLiteral("lightmap:reset"), "lightmap", {{"value", 1.0}}));
    v->addWidget(lm);

    density_ = new QLabel(body);
    density_->setObjectName(QStringLiteral("density"));
    v->addWidget(density_);

    auto* row3 = new QHBoxLayout;
    row3->addWidget(button(QStringLiteral("Copy UV"), QStringLiteral("copyUv"), "copyUv"));
    row3->addWidget(button(QStringLiteral("Paste UV"), QStringLiteral("pasteUv"), "pasteUv"));
    row3->addWidget(button(QStringLiteral("Flow"), QStringLiteral("flow"), "flow", {}, QStringLiteral("Continue the copied face texture onto the selected face")));
    row3->addWidget(button(QStringLiteral("Lift"), QStringLiteral("lift"), "lift", {}, QStringLiteral("Make the texture of the selected face the active one")));
    v->addLayout(row3);
    v->addStretch(1);
}

QWidget* SurfacePanel::part(const QString& name) const { return findChild<QWidget*>(name); }

void SurfacePanel::refreshNow() {
    timer_->stop();
    fill();
}

void SurfacePanel::fill() {
    const auto d = win_.document();
    fields_ = d ? tools::tex::surfaceFields(*d, win_.selection().items(), win_.textureInfo()) : tools::tex::SurfaceFields{};
    const tools::tex::SurfaceFields& f = fields_;
    filling_ = true;
    const auto setSpin = [](QDoubleSpinBox* s, double value) {
        if (s->hasFocus()) return;   // being typed in
        s->setValue(value);
    };
    if (!f.valid) {
        title_->setText(QStringLiteral("Select a face or a brush to edit its surface. The active texture is ") + qs(win_.activeTexture()) + QStringLiteral("."));
        if (!texture_->hasFocus()) texture_->setText(qs(win_.activeTexture()));
    } else {
        QString t = f.faceCount == 1 ? QStringLiteral("1 face") : QString::number(f.faceCount) + QStringLiteral(" faces");
        if (f.mixedTexture) t += QStringLiteral(" (different textures)");
        title_->setText(t);
        if (!texture_->hasFocus()) texture_->setText(f.mixedTexture ? QString() : qs(f.texture));
        for (int a = 0; a < 2; ++a) {
            setSpin(shift_[a], f.shift[a]);
            setSpin(stretch_[a], f.stretch[a]);
            mode_[a]->setCurrentIndex(f.repeatMode[a] == "repeat" ? 1 : f.repeatMode[a] == "mirror" ? 2 : 0);
            edge_[a]->setCurrentIndex(f.edge[a] == "wrap" ? 1 : f.edge[a] == "clamp" ? 2 : 0);
        }
        setSpin(rotate_, f.rotate);
        setSpin(shear_, f.shear);
        setSpin(lightmap_, f.lightmap);
    }
    density_->setText(f.valid && !f.density.empty() ? QStringLiteral("Texel density: ") + qs(f.density) : QString());
    filling_ = false;
}

bool SurfacePanel::setField(const QString& field, double value) {
    const int a = field.endsWith(QLatin1Char('V')) ? 1 : 0;
    if (field.startsWith(QLatin1String("shift"))) return win_.textureCommand("shift", {{"axis", a}, {"value", value}});
    if (field.startsWith(QLatin1String("stretch"))) {
        if (std::abs(value) < 1e-6) return false;
        return win_.textureCommand("stretch", {{"axis", a}, {"value", value}});
    }
    if (field == QLatin1String("rotate")) return win_.textureCommand("rotate", {{"value", value}});
    if (field == QLatin1String("shear")) return win_.textureCommand("shear", {{"value", value}});
    if (field == QLatin1String("lightmap")) return win_.textureCommand("lightmap", {{"value", value}});
    return false;
}

bool SurfacePanel::setTexture(const QString& texture) {
    const QString t = texture.trimmed();
    if (t.isEmpty()) return false;
    return win_.textureCommand("apply", {{"texture", t.toStdString()}});
}

void SurfacePanel::setRepeat(int axis, bool edge, const QString& value) {
    nlohmann::json p = {{"axis", axis}};
    p[edge ? "edge" : "mode"] = value.toStdString();
    win_.textureCommand("repeat", p);
}

} // namespace native::ui
