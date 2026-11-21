// ui/texture_props_window -- see texture_props_window.h.
#include "ui/texture_props_window.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "tools/texturing/props_edit.h"
#include "ui/main_window.h"

namespace native::ui {

namespace tex = tools::tex;
using json = nlohmann::json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

void addItems(QComboBox* c, std::initializer_list<const char*> labels) {
    for (const char* l : labels) c->addItem(QString::fromLatin1(l));
}

const char* kModes[] = {"repeat", "mirror", "anchor"};
const char* kAnchors[] = {"start", "end"};
const char* kFills[] = {"repeat", "mirror"};
const char* kEdges[] = {"auto", "wrap", "clamp"};
const char* kTrans[] = {"auto", "opaque", "cutout", "translucent"};

template <size_t N>
int indexOf(const char* (&names)[N], const std::string& v) {
    for (size_t i = 0; i < N; ++i)
        if (v == names[i]) return static_cast<int>(i);
    return 0;
}

QDoubleSpinBox* spinBox(QWidget* parent, const QString& name, double lo, double hi, int decimals, double step) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(name);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setKeyboardTracking(false);
    return s;
}

} // namespace

// ---- the hotspot canvas ---------------------------------------------------------------------------------------------------------------

HotspotCanvas::HotspotCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("canvas"));
    setMinimumSize(200, 200);
}

QRectF HotspotCanvas::imageRect() const {
    const double margin = 10;
    const double aw = std::max(10.0, width() - 2 * margin), ah = std::max(10.0, height() - 2 * margin);
    double w = aw, h = ah;
    if (!image_.isNull() && image_.width() > 0 && image_.height() > 0) {
        const double k = std::min(aw / image_.width(), ah / image_.height());
        w = image_.width() * k;
        h = image_.height() * k;
    } else {
        w = h = std::min(aw, ah);
    }
    return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
}

QPointF HotspotCanvas::toImage(const QPointF& p) const {
    const QRectF r = imageRect();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((p.y() - r.top()) / r.height(), 0.0, 1.0)};
}

QPointF HotspotCanvas::toPixel(const QPointF& f) const {
    const QRectF r = imageRect();
    return {r.left() + f.x() * r.width(), r.top() + f.y() * r.height()};
}

void HotspotCanvas::setHotspots(const std::vector<tex::Hotspot>& h, int selected) {
    hotspots_ = h;
    selected_ = selected < static_cast<int>(h.size()) ? selected : -1;
    update();
}

void HotspotCanvas::select(int index) {
    selected_ = index >= 0 && index < static_cast<int>(hotspots_.size()) ? index : -1;
    update();
    emit changed(hotspots_, selected_);
}

void HotspotCanvas::press(const QPointF& p) {
    const QPointF f = toImage(p);
    const int hit = tex::hotspotAt(hotspots_, f.x(), f.y());
    start_ = f;
    drawIndex_ = -1;
    if (hit >= 0) {
        selected_ = hit;
        startRect_ = hotspots_[static_cast<size_t>(hit)];
        mode_ = Mode::Move;
    } else {
        selected_ = -1;
        mode_ = Mode::Draw;
    }
    update();
    emit changed(hotspots_, selected_);
}

void HotspotCanvas::move(const QPointF& p) {
    if (mode_ == Mode::None) return;
    const QPointF f = toImage(p);
    if (mode_ == Mode::Move && selected_ >= 0) {
        hotspots_[static_cast<size_t>(selected_)] = tex::moveRect(startRect_, f.x() - start_.x(), f.y() - start_.y());
    } else if (mode_ == Mode::Draw) {
        if (drawIndex_ < 0 && std::hypot(f.x() - start_.x(), f.y() - start_.y()) < 0.01) return;   // a click, not a drag
        const tex::Hotspot r = tex::rectFromCorners(start_.x(), start_.y(), f.x(), f.y());
        if (drawIndex_ < 0) {
            if (hotspots_.size() >= tex::kMaxHotspots) return;
            hotspots_.push_back(r);
            drawIndex_ = static_cast<int>(hotspots_.size()) - 1;
            selected_ = drawIndex_;
        } else {
            hotspots_[static_cast<size_t>(drawIndex_)] = r;
        }
    }
    update();
    emit changed(hotspots_, selected_);
}

void HotspotCanvas::release() {
    if (mode_ == Mode::None) return;
    mode_ = Mode::None;
    drawIndex_ = -1;
    emit finished();
}

void HotspotCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) press(e->position());
}
void HotspotCanvas::mouseMoveEvent(QMouseEvent* e) { move(e->position()); }
void HotspotCanvas::mouseReleaseEvent(QMouseEvent*) { release(); }

void HotspotCanvas::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), QColor(0x16, 0x16, 0x16));
    const QRectF r = imageRect();
    g.fillRect(r, QColor(0x2a, 0x2a, 0x2a));
    if (!image_.isNull()) g.drawImage(r, image_);
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const tex::Hotspot& h = hotspots_[i];
        const QRectF hr(r.left() + h.x * r.width(), r.top() + h.y * r.height(), h.w * r.width(), h.h * r.height());
        const bool sel = static_cast<int>(i) == selected_;
        g.setPen(QPen(sel ? QColor(0xff, 0xd3, 0x4d) : QColor(0xf0, 0xa0, 0x30), sel ? 2 : 1));
        g.setBrush(QColor(240, 160, 48, sel ? 80 : 40));
        g.drawRect(hr);
        g.setPen(Qt::white);
        g.drawText(hr.topLeft() + QPointF(3, 12), QString::number(i + 1));
    }
}

// ---- the dialog -----------------------------------------------------------------------------------------------------------------------

TexturePropsDialog::TexturePropsDialog(MainWindow& window, const std::string& texture) : QDialog(&window, Qt::Tool), win_(window), texture_(texture) {
    setObjectName(QStringLiteral("texturePropsDialog"));
    setWindowTitle(QStringLiteral("Texture properties: ") + qs(texture));
    setModal(false);
    original_ = tex::normalizedProps(win_.textureProps(texture));
    draft_ = original_;
    build();
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(350);
    QObject::connect(saveTimer_, &QTimer::timeout, this, [this] { flush(); });
    QObject::connect(this, &QDialog::finished, this, [this](int) { flush(); });
    fill();
}

TexturePropsDialog::~TexturePropsDialog() = default;

QWidget* TexturePropsDialog::part(const QString& name) const { return findChild<QWidget*>(name); }

QString TexturePropsDialog::status() const { return status_->text(); }

void TexturePropsDialog::build() {
    auto* outer = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    outer->addLayout(left, 1);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Pixels per metre"), this));
    ppm_ = spinBox(this, QStringLiteral("ppm"), 0, tex::kPpmMax, 1, 8);
    ppm_->setSpecialValueText(QStringLiteral("default"));
    top->addWidget(ppm_);
    for (const double preset : tex::kPpmPresets) {
        auto* b = new QPushButton(QString::number(preset), this);
        b->setObjectName(QStringLiteral("btn:ppm:%1").arg(preset));
        b->setFocusPolicy(Qt::NoFocus);
        QObject::connect(b, &QPushButton::clicked, this, [this, preset] { ppm_->setValue(preset); });
        top->addWidget(b);
    }
    top->addStretch(1);
    left->addLayout(top);
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("info"));
    left->addWidget(info_);

    for (int a = 0; a < 2; ++a) {
        const QString ax = a == 0 ? QStringLiteral("U") : QStringLiteral("V");
        auto* box = new QGroupBox(a == 0 ? QStringLiteral("Horizontal (U)") : QStringLiteral("Vertical (V)"), this);
        auto* g = new QGridLayout(box);
        mode_[a] = new QComboBox(box);
        mode_[a]->setObjectName(QStringLiteral("mode") + ax);
        addItems(mode_[a], {"Repeat", "Mirror", "Anchor"});
        anchor_[a] = new QComboBox(box);
        anchor_[a]->setObjectName(QStringLiteral("anchor") + ax);
        if (a == 0) addItems(anchor_[a], {"Left", "Right"});
        else addItems(anchor_[a], {"Top", "Bottom"});
        split_[a] = spinBox(box, QStringLiteral("split") + ax, 0.02, 0.98, 3, 0.05);
        fill_[a] = new QComboBox(box);
        fill_[a]->setObjectName(QStringLiteral("fill") + ax);
        addItems(fill_[a], {"Repeat", "Mirror"});
        edge_[a] = new QComboBox(box);
        edge_[a]->setObjectName(QStringLiteral("edge") + ax);
        addItems(edge_[a], {"Auto", "Wrap", "Clamp"});
        alt_[a] = new QLineEdit(box);
        alt_[a]->setObjectName(QStringLiteral("alt") + ax);
        alt_[a]->setPlaceholderText(QStringLiteral("alternate texture (anchor mode)"));
        altOff_[a][0] = spinBox(box, QStringLiteral("altOff") + ax + QStringLiteral("0"), -8, 8, 3, 0.05);
        altOff_[a][1] = spinBox(box, QStringLiteral("altOff") + ax + QStringLiteral("1"), -8, 8, 3, 0.05);
        g->addWidget(new QLabel(QStringLiteral("Mode"), box), 0, 0);
        g->addWidget(mode_[a], 0, 1);
        g->addWidget(new QLabel(QStringLiteral("Edge"), box), 0, 2);
        g->addWidget(edge_[a], 0, 3);
        g->addWidget(new QLabel(QStringLiteral("Anchor"), box), 1, 0);
        g->addWidget(anchor_[a], 1, 1);
        g->addWidget(new QLabel(QStringLiteral("Split"), box), 1, 2);
        g->addWidget(split_[a], 1, 3);
        g->addWidget(new QLabel(QStringLiteral("Fill"), box), 2, 0);
        g->addWidget(fill_[a], 2, 1);
        g->addWidget(new QLabel(QStringLiteral("Alternate"), box), 3, 0);
        g->addWidget(alt_[a], 3, 1, 1, 3);
        g->addWidget(new QLabel(QStringLiteral("Offset"), box), 4, 0);
        g->addWidget(altOff_[a][0], 4, 1);
        g->addWidget(altOff_[a][1], 4, 2);
        auto* useActive = new QPushButton(QStringLiteral("Use active"), box);
        useActive->setObjectName(QStringLiteral("btn:alt") + ax);
        useActive->setFocusPolicy(Qt::NoFocus);
        QObject::connect(useActive, &QPushButton::clicked, this, [this, a] {
            alt_[a]->setText(qs(win_.activeTexture()));
            axisEdited(a);
        });
        g->addWidget(useActive, 4, 3);
        left->addWidget(box);
        const auto changed = [this, a] { if (!filling_) axisEdited(a); };
        QObject::connect(mode_[a], &QComboBox::currentIndexChanged, this, changed);
        QObject::connect(anchor_[a], &QComboBox::currentIndexChanged, this, changed);
        QObject::connect(fill_[a], &QComboBox::currentIndexChanged, this, changed);
        QObject::connect(edge_[a], &QComboBox::currentIndexChanged, this, changed);
        QObject::connect(split_[a], &QDoubleSpinBox::valueChanged, this, changed);
        QObject::connect(altOff_[a][0], &QDoubleSpinBox::valueChanged, this, changed);
        QObject::connect(altOff_[a][1], &QDoubleSpinBox::valueChanged, this, changed);
        QObject::connect(alt_[a], &QLineEdit::editingFinished, this, changed);
    }

    auto* tbox = new QGroupBox(QStringLiteral("Transparency"), this);
    auto* tl = new QHBoxLayout(tbox);
    transMode_ = new QComboBox(tbox);
    transMode_->setObjectName(QStringLiteral("transMode"));
    addItems(transMode_, {"Auto", "Opaque", "Cutout", "Translucent"});
    cutoff_ = spinBox(tbox, QStringLiteral("cutoff"), 0, 1, 2, 0.05);
    opacity_ = spinBox(tbox, QStringLiteral("opacity"), 0, 1, 2, 0.05);
    tl->addWidget(transMode_);
    tl->addWidget(new QLabel(QStringLiteral("Cutoff"), tbox));
    tl->addWidget(cutoff_);
    tl->addWidget(new QLabel(QStringLiteral("Opacity"), tbox));
    tl->addWidget(opacity_);
    left->addWidget(tbox);
    const auto transChanged = [this] {
        if (filling_) return;
        tex::TransparencyProps t;
        t.mode = kTrans[std::clamp(transMode_->currentIndex(), 0, 3)];
        t.cutoff = cutoff_->value();
        t.opacity = opacity_->value();
        draft_ = tex::withTransparency(draft_, t);
        edited();
    };
    QObject::connect(transMode_, &QComboBox::currentIndexChanged, this, transChanged);
    QObject::connect(cutoff_, &QDoubleSpinBox::valueChanged, this, transChanged);
    QObject::connect(opacity_, &QDoubleSpinBox::valueChanged, this, transChanged);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    left->addWidget(status_);
    auto* bottom = new QHBoxLayout;
    auto* revert = new QPushButton(QStringLiteral("Revert"), this);
    revert->setObjectName(QStringLiteral("btn:revert"));
    revert->setFocusPolicy(Qt::NoFocus);
    QObject::connect(revert, &QPushButton::clicked, this, [this] { setProps(original_); });
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    close->setObjectName(QStringLiteral("btn:close"));
    QObject::connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(revert);
    bottom->addStretch(1);
    bottom->addWidget(close);
    left->addLayout(bottom);

    // right: the picture with the hotspots
    auto* right = new QVBoxLayout;
    outer->addLayout(right, 1);
    right->addWidget(new QLabel(QStringLiteral("Hotspots: drag on the picture to draw a rectangle, drag one to move it"), this));
    canvas_ = new HotspotCanvas(this);
    canvas_->setImage(win_.loadTextureImage(texture_));
    right->addWidget(canvas_, 1);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("hotspots"));
    list_->setMaximumHeight(110);
    right->addWidget(list_);
    auto* rects = new QHBoxLayout;
    const char* names[] = {"hx", "hy", "hw", "hh"};
    for (int i = 0; i < 4; ++i) {
        rect_[i] = spinBox(this, QString::fromLatin1(names[i]), 0, 1, 4, 0.01);
        rects->addWidget(new QLabel(QString::fromLatin1(names[i] + 1).toUpper(), this));
        rects->addWidget(rect_[i]);
    }
    right->addLayout(rects);
    auto* hb = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Add"), this);
    add->setObjectName(QStringLiteral("btn:addHotspot"));
    auto* remove = new QPushButton(QStringLiteral("Remove"), this);
    remove->setObjectName(QStringLiteral("btn:removeHotspot"));
    add->setFocusPolicy(Qt::NoFocus);
    remove->setFocusPolicy(Qt::NoFocus);
    hb->addWidget(add);
    hb->addWidget(remove);
    hb->addStretch(1);
    right->addLayout(hb);

    QObject::connect(ppm_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (filling_) return;
        draft_ = tex::withPpm(draft_, v > 0 ? std::optional<double>(v) : std::nullopt);
        edited();
    });
    QObject::connect(canvas_, &HotspotCanvas::changed, this, [this](const std::vector<tex::Hotspot>& h, int sel) { hotspotsChanged(h, sel); });
    QObject::connect(canvas_, &HotspotCanvas::finished, this, [this] { saveTimer_->start(); });
    QObject::connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!filling_) canvas_->select(row);
    });
    for (int i = 0; i < 4; ++i)
        QObject::connect(rect_[i], &QDoubleSpinBox::valueChanged, this, [this](double) {
            if (filling_ || canvas_->selected() < 0) return;
            std::vector<tex::Hotspot> h = canvas_->hotspots();
            h[static_cast<size_t>(canvas_->selected())] = tex::rectFromCorners(rect_[0]->value(), rect_[1]->value(), rect_[0]->value() + rect_[2]->value(), rect_[1]->value() + rect_[3]->value());
            hotspotsChanged(h, canvas_->selected());
        });
    QObject::connect(add, &QPushButton::clicked, this, [this] {
        std::vector<tex::Hotspot> h = canvas_->hotspots();
        if (h.size() >= tex::kMaxHotspots) return;
        h.push_back({0.25, 0.25, 0.5, 0.5});
        hotspotsChanged(h, static_cast<int>(h.size()) - 1);
    });
    QObject::connect(remove, &QPushButton::clicked, this, [this] {
        std::vector<tex::Hotspot> h = canvas_->hotspots();
        const int sel = canvas_->selected();
        if (sel < 0 || sel >= static_cast<int>(h.size())) return;
        h.erase(h.begin() + sel);
        hotspotsChanged(h, -1);
    });
}

void TexturePropsDialog::fill() {
    filling_ = true;
    ppm_->setValue(tex::ppmOf(draft_).value_or(0));
    for (int a = 0; a < 2; ++a) {
        const tex::AxisProps p = tex::axisOf(draft_, a);
        mode_[a]->setCurrentIndex(indexOf(kModes, p.mode));
        anchor_[a]->setCurrentIndex(indexOf(kAnchors, p.anchor));
        split_[a]->setValue(p.split);
        fill_[a]->setCurrentIndex(indexOf(kFills, p.fill));
        edge_[a]->setCurrentIndex(indexOf(kEdges, p.edge));
        alt_[a]->setText(qs(p.altTexture));
        altOff_[a][0]->setValue(p.altOffset[0]);
        altOff_[a][1]->setValue(p.altOffset[1]);
        const bool anchored = p.mode == "anchor";
        for (QWidget* w : std::initializer_list<QWidget*>{anchor_[a], split_[a], fill_[a], alt_[a], altOff_[a][0], altOff_[a][1]}) w->setEnabled(anchored);
    }
    const tex::TransparencyProps t = tex::transparencyOf(draft_);
    transMode_->setCurrentIndex(indexOf(kTrans, t.mode));
    cutoff_->setValue(t.cutoff);
    opacity_->setValue(t.opacity);
    cutoff_->setEnabled(t.mode == "cutout");
    opacity_->setEnabled(t.mode == "translucent");
    filling_ = false;
    hotspotsChanged(tex::hotspotsOf(draft_), -1);
    filling_ = true;
    if (const core::texplan::TexInfo* i = win_.textureInfo() ? win_.textureInfo()(texture_) : nullptr)
        info_->setText(QStringLiteral("%1 x %2 px, %3 x %4 m at %5 px/m").arg(i->width).arg(i->height).arg(i->width / i->ppm, 0, 'f', 2).arg(i->height / i->ppm, 0, 'f', 2).arg(i->ppm, 0, 'f', 1));
    else
        info_->setText(QStringLiteral("(texture not loaded)"));
    filling_ = false;
}

void TexturePropsDialog::setProps(const json& props) {
    draft_ = tex::normalizedProps(props);
    fill();
    edited();
}

void TexturePropsDialog::edited() {
    draft_ = tex::normalizedProps(draft_);
    dirty_ = true;
    status_->setText(QStringLiteral("Unsaved..."));
    saveTimer_->start();
}

void TexturePropsDialog::axisEdited(int axis) {
    tex::AxisProps p;
    p.mode = kModes[std::clamp(mode_[axis]->currentIndex(), 0, 2)];
    p.anchor = kAnchors[std::clamp(anchor_[axis]->currentIndex(), 0, 1)];
    p.split = split_[axis]->value();
    p.fill = kFills[std::clamp(fill_[axis]->currentIndex(), 0, 1)];
    p.edge = kEdges[std::clamp(edge_[axis]->currentIndex(), 0, 2)];
    p.altTexture = alt_[axis]->text().trimmed().toStdString();
    p.altOffset = {altOff_[axis][0]->value(), altOff_[axis][1]->value()};
    draft_ = tex::withAxis(draft_, axis, p);
    edited();
    const bool anchored = p.mode == "anchor";
    for (QWidget* w : std::initializer_list<QWidget*>{anchor_[axis], split_[axis], fill_[axis], alt_[axis], altOff_[axis][0], altOff_[axis][1]}) w->setEnabled(anchored);
}

void TexturePropsDialog::hotspotsChanged(const std::vector<tex::Hotspot>& h, int selected) {
    const bool wasFilling = filling_;
    filling_ = true;
    const std::vector<tex::Hotspot> before = tex::hotspotsOf(draft_);
    if (!(before == h)) {
        draft_ = tex::withHotspots(draft_, h);
        if (!wasFilling) edited();
    }
    canvas_->setHotspots(h, selected);
    {
        QSignalBlocker block(list_);
        list_->clear();
        for (size_t i = 0; i < h.size(); ++i)
            list_->addItem(QStringLiteral("%1: %2, %3  %4 x %5").arg(i + 1).arg(h[i].x, 0, 'f', 3).arg(h[i].y, 0, 'f', 3).arg(h[i].w, 0, 'f', 3).arg(h[i].h, 0, 'f', 3));
        list_->setCurrentRow(selected);
    }
    const bool any = selected >= 0 && selected < static_cast<int>(h.size());
    for (int i = 0; i < 4; ++i) {
        rect_[i]->setEnabled(any);
        if (!any) continue;
        const tex::Hotspot& r = h[static_cast<size_t>(selected)];
        const double v[4] = {r.x, r.y, r.w, r.h};
        if (!rect_[i]->hasFocus()) rect_[i]->setValue(v[i]);
    }
    filling_ = wasFilling;
}

std::string TexturePropsDialog::flush() {
    saveTimer_->stop();
    if (!dirty_) return {};
    dirty_ = false;
    const std::string err = win_.saveTextureProps(texture_, draft_);
    status_->setText(err.empty() ? QStringLiteral("Saved next to the texture") : QStringLiteral("Could not save: ") + qs(err));
    if (!err.empty()) dirty_ = true;
    return err;
}

} // namespace native::ui
