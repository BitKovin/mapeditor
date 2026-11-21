// ui/texgroup_editor -- see texgroup_editor.h.
#include "ui/texgroup_editor.h"

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "core/texplan.h"
#include "tools/texturing/group_tools.h"
#include "ui/main_window.h"

namespace native::ui {

namespace tex = native::tools::tex;
namespace tp = core::texplan;
using ojson = nlohmann::ordered_json;

namespace {

constexpr double kMargin = 8;

int hueOf(const std::string& s) {
    int x = 0;
    for (const char c : s) x = (x * 31 + static_cast<unsigned char>(c)) % 360;
    return x;
}

QString fmt(double v) { return QString::number(v, 'g', 4); }

double tiltOf(const QString& facing) { return facing == QLatin1String("up") ? 0 : facing == QLatin1String("down") ? 180 : 90; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
GroupPreviewCanvas::GroupPreviewCanvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(260, 220);
    setObjectName(QStringLiteral("preview"));
}

void GroupPreviewCanvas::setSources(InfoSource info, ImageSource image) {
    info_ = std::move(info);
    imageOf_ = std::move(image);
    images_.clear();
    update();
}

void GroupPreviewCanvas::setLayout(std::vector<tp::Cell> cells, double w, double h) {
    cells_ = std::move(cells);
    w_ = w;
    h_ = h;
    if (highlight_ >= static_cast<int>(cells_.size())) highlight_ = -1;
    if (hovered_ >= static_cast<int>(cells_.size())) hovered_ = -1;
    update();
}

void GroupPreviewCanvas::setHighlight(int index) {
    if (index >= static_cast<int>(cells_.size())) index = -1;
    if (index == highlight_) return;
    highlight_ = index;
    update();
    emit highlightChanged(highlight_);
}

const QImage& GroupPreviewCanvas::image(const std::string& path) {
    const auto it = images_.find(path);
    if (it != images_.end()) return it->second;
    return images_[path] = imageOf_ ? imageOf_(path) : QImage();
}

QPointF GroupPreviewCanvas::pixelOf(double x, double y) const {
    const double s = tex::previewScale(width(), height(), w_, h_, kMargin);
    return {kMargin + s * x, kMargin + s * (h_ - y)};
}

int GroupPreviewCanvas::cellAtPixel(const QPointF& p) const {
    const double s = tex::previewScale(width(), height(), w_, h_, kMargin);
    if (!(s > 0)) return -1;
    return tex::cellAt(cells_, (p.x() - kMargin) / s, h_ - (p.y() - kMargin) / s);
}

void GroupPreviewCanvas::hover(const QPointF& p) {
    const int i = cellAtPixel(p);
    if (i == hovered_) return;
    hovered_ = i;
    emit hoveredChanged(i);
}

void GroupPreviewCanvas::click(const QPointF& p) {
    const int i = cellAtPixel(p);
    setHighlight(i == highlight_ ? -1 : i);
}

void GroupPreviewCanvas::mouseMoveEvent(QMouseEvent* e) { hover(e->position()); }
void GroupPreviewCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) click(e->position());
}
void GroupPreviewCanvas::leaveEvent(QEvent*) {
    if (hovered_ < 0) return;
    hovered_ = -1;
    emit hoveredChanged(-1);
}

void GroupPreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), QColor(0x16, 0x16, 0x16));
    const double s = tex::previewScale(width(), height(), w_, h_, kMargin);
    if (!(s > 0)) return;
    g.setRenderHint(QPainter::SmoothPixmapTransform, false);
    g.translate(kMargin, kMargin);
    for (const tp::Cell& c : cells_) {
        const QRectF r(s * c.x0, s * (h_ - c.y1), s * (c.x1 - c.x0), s * (c.y1 - c.y0));
        const tp::TexInfo* ti = info_ ? info_(c.texture) : nullptr;
        const QImage& img = image(c.texture);
        g.save();
        g.setClipRect(r);
        if (ti && !img.isNull() && ti->width > 0 && ti->height > 0) {
            const auto m = tex::previewPatternMatrix(tp::cellMap(c, tp::naturalSize(*ti), w_, h_), ti->width, ti->height, s, h_);
            QBrush brush(img);
            brush.setTransform(QTransform(m[0], m[1], m[2], m[3], m[4], m[5]));
            g.fillRect(r, brush);
        } else {
            g.fillRect(r, QColor::fromHsl(hueOf(c.slot.empty() ? c.texture : c.slot), 64, 76));
        }
        g.restore();
        if (outlines_) {
            g.setPen(QPen(QColor(255, 255, 255, 90), 1));
            g.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        if (labels_ && r.width() > 34 && r.height() > 14) {
            const QString label = QString::fromStdString(c.slot.empty() ? "fallback" : c.slot);
            const double tw = std::min(r.width() - 4, static_cast<double>(g.fontMetrics().horizontalAdvance(label)) + 6);
            g.fillRect(QRectF(r.x() + 2, r.y() + 2, tw, 13), QColor(0, 0, 0, 150));
            g.setPen(Qt::white);
            g.drawText(QPointF(r.x() + 5, r.y() + 12), label);
        }
    }
    if (highlight_ >= 0 && highlight_ < static_cast<int>(cells_.size())) {
        const tp::Cell& c = cells_[static_cast<size_t>(highlight_)];
        g.setPen(QPen(QColor(0xff, 0xd3, 0x4d), 2.5));
        g.drawRect(QRectF(s * c.x0 + 1, s * (h_ - c.y1) + 1, s * (c.x1 - c.x0) - 2, s * (c.y1 - c.y0) - 2));
    }
    g.setPen(QPen(QColor(0x0a, 0x84, 0xe8), 1.5));
    g.drawRect(QRectF(0, 0, s * w_, s * h_));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
TexGroupEditor::TexGroupEditor(MainWindow& window, const std::string& name) : QDialog(&window), win_(window), original_(name) {
    setObjectName(QStringLiteral("TexGroupEditor"));
    setWindowTitle(name.empty() ? QStringLiteral("New texture group") : QString::fromStdString("Texture group: " + name));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1120, 700);
    build();
    QString initial;
    bool unreadable = false;
    if (name.empty()) {
        initial = QString::fromStdString(tex::groupTemplates().front().text);
    } else if (const auto raw = win_.texGroupRead(name)) {
        initial = QString::fromStdString(ojson::parse(raw->dump()).dump(2));
    } else {
        initial = QString::fromStdString(tex::groupTemplates().front().text);
        unreadable = true;
    }
    filling_ = true;
    text_->setPlainText(initial);
    filling_ = false;
    reparse();
    if (unreadable) status_->setText(QString::fromStdString("Could not read '" + name + "'"));
}

TexGroupEditor::~TexGroupEditor() = default;

void TexGroupEditor::build() {
    auto* root = new QVBoxLayout(this);
    // the bar
    auto* bar = new QHBoxLayout;
    name_ = new QLineEdit(QString::fromStdString(original_));
    name_->setObjectName(QStringLiteral("name"));
    name_->setPlaceholderText(QStringLiteral("folder/name (no extension)"));
    name_->setMinimumWidth(240);
    name_->setReadOnly(!original_.empty());
    template_ = new QComboBox;
    template_->setObjectName(QStringLiteral("template"));
    template_->setToolTip(QStringLiteral("Replace the text with a starter group"));
    template_->addItem(QStringLiteral("Template..."));
    for (const auto& t : tex::groupTemplates()) template_->addItem(QString::fromStdString(t.label));
    auto* saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setObjectName(QStringLiteral("btn:save"));
    saveBtn->setDefault(true);
    auto* applyBtn = new QPushButton(QStringLiteral("Apply to selection"));
    applyBtn->setObjectName(QStringLiteral("btn:apply"));
    applyBtn->setToolTip(QStringLiteral("Save, then lay this group over the selected faces / brushes"));
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("status"));
    bar->addWidget(name_);
    bar->addWidget(template_);
    bar->addWidget(saveBtn);
    bar->addWidget(applyBtn);
    bar->addWidget(status_, 1);
    root->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal);
    // left: the text
    text_ = new QPlainTextEdit;
    text_->setObjectName(QStringLiteral("text"));
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setTabStopDistance(2 * text_->fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    split->addWidget(text_);
    // right: the preview and its controls
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    auto* controls = new QHBoxLayout;
    w_ = new QDoubleSpinBox;
    w_->setObjectName(QStringLiteral("width"));
    w_->setRange(0.1, 200);
    w_->setDecimals(2);
    w_->setSingleStep(0.5);
    w_->setValue(9);
    w_->setPrefix(QStringLiteral("width "));
    w_->setSuffix(QStringLiteral(" m"));
    h_ = new QDoubleSpinBox;
    h_->setObjectName(QStringLiteral("height"));
    h_->setRange(0.1, 200);
    h_->setDecimals(2);
    h_->setSingleStep(0.5);
    h_->setValue(12);
    h_->setPrefix(QStringLiteral("height "));
    h_->setSuffix(QStringLiteral(" m"));
    facing_ = new QComboBox;
    facing_->setObjectName(QStringLiteral("facing"));
    facing_->addItem(QStringLiteral("wall"), QStringLiteral("wall"));
    facing_->addItem(QStringLiteral("floor / roof (up)"), QStringLiteral("up"));
    facing_->addItem(QStringLiteral("ceiling (down)"), QStringLiteral("down"));
    seed_ = new QSpinBox;
    seed_->setObjectName(QStringLiteral("seed"));
    seed_->setRange(0, 999999999);
    seed_->setValue(1);
    seed_->setPrefix(QStringLiteral("seed "));
    auto* reroll = new QPushButton(QStringLiteral("Re-roll"));
    reroll->setObjectName(QStringLiteral("btn:reroll"));
    outlines_ = new QCheckBox(QStringLiteral("outlines"));
    outlines_->setObjectName(QStringLiteral("outlines"));
    outlines_->setChecked(true);
    labels_ = new QCheckBox(QStringLiteral("labels"));
    labels_->setObjectName(QStringLiteral("labels"));
    labels_->setChecked(true);
    for (QWidget* c : std::initializer_list<QWidget*>{w_, h_, facing_, seed_, reroll, outlines_, labels_}) controls->addWidget(c);
    controls->addStretch(1);
    rv->addLayout(controls);
    paramBox_ = new QWidget;
    paramBox_->setObjectName(QStringLiteral("params"));
    auto* pf = new QFormLayout(paramBox_);
    pf->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(paramBox_);
    preview_ = new GroupPreviewCanvas;
    preview_->setSources([this](const std::string& p) { return win_.textureInfo() ? win_.textureInfo()(p) : nullptr; },
                         [this](const std::string& p) { return win_.loadTextureImage(p); });
    rv->addWidget(preview_, 1);
    info_ = new QLabel(QStringLiteral("Hover the preview to see which rule made a part; click to mark it."));
    info_->setObjectName(QStringLiteral("info"));
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setWordWrap(true);
    rv->addWidget(info_);
    slotList_ = new QListWidget;
    slotList_->setObjectName(QStringLiteral("slots"));
    slotList_->setMaximumHeight(110);
    rv->addWidget(slotList_);
    auto* slotRow = new QHBoxLayout;
    slotTexture_ = new QLineEdit;
    slotTexture_->setObjectName(QStringLiteral("slotTexture"));
    slotTexture_->setPlaceholderText(QStringLiteral("texture path of the selected slot"));
    auto* slotSet = new QPushButton(QStringLiteral("Set"));
    slotSet->setObjectName(QStringLiteral("btn:slotSet"));
    slotSet->setToolTip(QStringLiteral("Use the typed texture for the selected slot (the text is reformatted)"));
    auto* slotActive = new QPushButton(QStringLiteral("Use active texture"));
    slotActive->setObjectName(QStringLiteral("btn:slotActive"));
    slotActive->setToolTip(QStringLiteral("Use the texture chosen in the Textures panel for the selected slot"));
    slotRow->addWidget(slotTexture_, 1);
    slotRow->addWidget(slotSet);
    slotRow->addWidget(slotActive);
    rv->addLayout(slotRow);
    problems_ = new QListWidget;
    problems_->setObjectName(QStringLiteral("problems"));
    problems_->setMaximumHeight(120);
    problems_->setSelectionMode(QAbstractItemView::NoSelection);
    rv->addWidget(problems_);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(200);
    connect(timer_, &QTimer::timeout, this, &TexGroupEditor::reparse);
    connect(text_, &QPlainTextEdit::textChanged, this, [this] {
        if (!filling_) timer_->start();
    });
    connect(template_, &QComboBox::activated, this, [this](int i) {
        if (i > 0) applyTemplate(i - 1);
        template_->setCurrentIndex(0);
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(applyBtn, &QPushButton::clicked, this, [this] { saveAndApply(); });
    const auto view = [this] { redraw(); };
    connect(w_, &QDoubleSpinBox::valueChanged, this, view);
    connect(h_, &QDoubleSpinBox::valueChanged, this, view);
    connect(facing_, &QComboBox::currentIndexChanged, this, view);
    connect(seed_, &QSpinBox::valueChanged, this, view);
    connect(reroll, &QPushButton::clicked, this, [this] { seed_->setValue(static_cast<int>(QRandomGenerator::global()->bounded(1000000))); });
    connect(outlines_, &QCheckBox::toggled, preview_, &GroupPreviewCanvas::setOutlines);
    connect(labels_, &QCheckBox::toggled, preview_, &GroupPreviewCanvas::setLabels);
    connect(preview_, &GroupPreviewCanvas::hoveredChanged, this, [this](int i) {
        if (preview_->highlight() < 0) info_->setText(describe(i).isEmpty() ? QStringLiteral("Hover the preview to see which rule made a part; click to mark it.") : describe(i));
    });
    connect(preview_, &GroupPreviewCanvas::highlightChanged, this, [this](int i) {
        info_->setText(describe(i).isEmpty() ? QStringLiteral("Hover the preview to see which rule made a part; click to mark it.") : describe(i));
    });
    connect(slotList_, &QListWidget::currentRowChanged, this, [this] { slotSelected(); });
    connect(slotSet, &QPushButton::clicked, this, [this] {
        const int row = slotList_->currentRow();
        if (row >= 0 && row < static_cast<int>(slots_.size()) && !slotTexture_->text().trimmed().isEmpty()) setSlot(slots_[static_cast<size_t>(row)].first, slotTexture_->text().trimmed().toStdString());
    });
    connect(slotActive, &QPushButton::clicked, this, [this] {
        const int row = slotList_->currentRow();
        if (row >= 0 && row < static_cast<int>(slots_.size())) setSlot(slots_[static_cast<size_t>(row)].first, win_.activeTexture());
    });
}

QString TexGroupEditor::text() const { return text_->toPlainText(); }

void TexGroupEditor::setText(const QString& text) {
    filling_ = true;
    text_->setPlainText(text);
    filling_ = false;
    timer_->stop();
    reparse();
}

void TexGroupEditor::setName(const QString& name) {
    if (original_.empty()) name_->setText(name);
}

void TexGroupEditor::applyTemplate(int index) {
    const auto& t = tex::groupTemplates();
    if (index < 0 || index >= static_cast<int>(t.size())) return;
    setText(QString::fromStdString(t[static_cast<size_t>(index)].text));
}

void TexGroupEditor::setSlot(const std::string& slot, const std::string& path) {
    const auto next = tex::setSlotTexture(text_->toPlainText().toStdString(), slot, path);
    if (!next) {
        status_->setText(QStringLiteral("The text must be valid JSON to change a slot"));
        return;
    }
    setText(QString::fromStdString(*next));
}

void TexGroupEditor::reparse() {
    timer_->stop();
    errors_.clear();
    warnings_.clear();
    valid_ = false;
    group_.reset();
    const ojson j = ojson::parse(text_->toPlainText().toStdString(), nullptr, false);
    if (j.is_discarded()) {
        errors_.push_back("The text is not valid JSON");
    } else if (!j.is_object()) {
        errors_.push_back("A group must be a JSON object");
    } else {
        parsed_ = j;
        tp::GroupParse p = tp::parseGroup(j);
        errors_ = p.errors;
        warnings_ = p.warnings;
        group_ = p.group;
        valid_ = group_ && errors_.empty();
    }
    rebuildParams();
    rebuildSlots();
    preview_->setHighlight(-1);
    redraw();
}

void TexGroupEditor::rebuildParams() {
    const std::vector<tex::ParamDef> defs = parsed_.is_object() && valid_ ? tex::paramDefsOf(parsed_) : std::vector<tex::ParamDef>{};
    std::vector<std::string> keys;
    for (const auto& d : defs) keys.push_back(d.name + ":" + d.type + ":" + std::to_string(d.options.size()));
    if (!valid_ && !paramKeys_.empty()) return;   // keep the controls while the text is broken
    if (keys == paramKeys_) return;
    paramKeys_ = keys;
    auto* form = static_cast<QFormLayout*>(paramBox_->layout());
    while (form->rowCount() > 0) form->removeRow(0);
    std::map<std::string, tp::ParamValue> kept;
    for (const auto& d : defs) {
        tp::ParamValue v;
        const auto old = values_.find(d.name);
        const bool reuse = old != values_.end() && ((d.type == "bool" && old->second.t == tp::ParamValue::T::Bool) || (d.type == "choice" && old->second.t == tp::ParamValue::T::Str) ||
                                                      ((d.type == "number" || d.type == "int") && old->second.t == tp::ParamValue::T::Num));
        if (d.type == "bool") {
            v.t = tp::ParamValue::T::Bool;
            v.b = reuse ? old->second.b : d.defBool;
        } else if (d.type == "choice") {
            v.t = tp::ParamValue::T::Str;
            v.s = reuse && std::find(d.options.begin(), d.options.end(), old->second.s) != d.options.end() ? old->second.s : d.defChoice;
        } else {
            v.t = tp::ParamValue::T::Num;
            v.n = reuse ? std::clamp(old->second.n, d.min, d.max) : d.defNumber;
        }
        kept[d.name] = v;
        QWidget* control = nullptr;
        const QString label = QString::fromStdString(d.label);
        const std::string pname = d.name;
        if (d.type == "bool") {
            auto* c = new QCheckBox;
            c->setChecked(v.b);
            connect(c, &QCheckBox::toggled, this, [this, pname](bool on) {
                values_[pname].t = tp::ParamValue::T::Bool;
                values_[pname].b = on;
                redraw();
            });
            control = c;
        } else if (d.type == "choice") {
            auto* c = new QComboBox;
            for (const auto& o : d.options) c->addItem(QString::fromStdString(o));
            c->setCurrentText(QString::fromStdString(v.s));
            connect(c, &QComboBox::currentTextChanged, this, [this, pname](const QString& t) {
                values_[pname].t = tp::ParamValue::T::Str;
                values_[pname].s = t.toStdString();
                redraw();
            });
            control = c;
        } else {
            auto* c = new QDoubleSpinBox;
            c->setDecimals(d.type == "int" ? 0 : 3);
            c->setRange(d.min, d.max);
            c->setSingleStep(d.step);
            c->setValue(v.n);
            connect(c, &QDoubleSpinBox::valueChanged, this, [this, pname](double x) {
                values_[pname].t = tp::ParamValue::T::Num;
                values_[pname].n = x;
                redraw();
            });
            control = c;
        }
        control->setObjectName(QString::fromStdString("param:" + d.name));
        form->addRow(label, control);
    }
    values_ = std::move(kept);
}

void TexGroupEditor::rebuildSlots() {
    const int keep = slotList_->currentRow();
    slots_ = parsed_.is_object() ? tex::slotsOf(parsed_) : std::vector<std::pair<std::string, std::string>>{};
    slotList_->clear();
    for (const auto& [slot, path] : slots_) slotList_->addItem(QString::fromStdString(slot + "   " + path));
    if (!slots_.empty()) slotList_->setCurrentRow(std::clamp(keep, 0, static_cast<int>(slots_.size()) - 1));
    slotSelected();
}

void TexGroupEditor::slotSelected() {
    const int row = slotList_->currentRow();
    slotTexture_->setText(row >= 0 && row < static_cast<int>(slots_.size()) ? QString::fromStdString(slots_[static_cast<size_t>(row)].second) : QString());
}

void TexGroupEditor::redraw() {
    solveWarnings_.clear();
    if (group_ && valid_) {
        tp::LayoutInput in;
        in.group = group_.get();
        in.params = values_;
        in.w = w_->value();
        in.h = h_->value();
        const QString facing = facing_->currentData().toString();
        in.facing = facing.toStdString();
        in.tilt = tiltOf(facing);
        in.seed = static_cast<uint32_t>(seed_->value());
        in.info = win_.textureInfo();
        tp::LayoutResult r = tp::solveLayout(in);
        solveWarnings_ = r.warnings;
        if (r.truncated) solveWarnings_.push_back("The layout was cut: it has too many parts");
        preview_->setLayout(std::move(r.cells), in.w, in.h);
        status_->setText(QString::number(preview_->cells().size()) + QStringLiteral(" cells"));
        // textures the group names that the assets folder does not have
        if (const auto info = win_.textureInfo())
            for (const auto& [slot, path] : slots_)
                if (!info(path)) solveWarnings_.push_back("texture '" + path + "' was not found in the assets folder");
    } else {
        preview_->setLayout({}, w_->value(), h_->value());
    }
    showProblems();
}

void TexGroupEditor::showProblems() {
    problems_->clear();
    for (const auto& e : errors_) {
        auto* it = new QListWidgetItem(QString::fromStdString(e), problems_);
        it->setForeground(QColor(0xff, 0x6b, 0x6b));
        it->setData(Qt::UserRole, QStringLiteral("error"));
    }
    for (const auto* list : {&warnings_, &solveWarnings_})
        for (const auto& w : *list) {
            auto* it = new QListWidgetItem(QString::fromStdString(w), problems_);
            it->setForeground(QColor(0xe8, 0xb3, 0x39));
            it->setData(Qt::UserRole, QStringLiteral("warning"));
        }
    if (problems_->count() == 0) problems_->addItem(QStringLiteral("No problems found."));
}

QString TexGroupEditor::describe(int i) const {
    const auto& cells = preview_->cells();
    if (i < 0 || i >= static_cast<int>(cells.size())) return {};
    const tp::Cell& c = cells[static_cast<size_t>(i)];
    return QStringLiteral("%1: %2 x %3 m at (%4, %5), fit %6, rule path %7, %8")
        .arg(c.slot.empty() ? QStringLiteral("fallback") : QString::fromStdString(c.slot), fmt(c.x1 - c.x0), fmt(c.y1 - c.y0), fmt(c.x0), fmt(c.y0), QString::fromStdString(c.fit),
             QString::fromStdString(c.path), QString::fromStdString(c.texture));
}

std::string TexGroupEditor::save() {
    const std::string name = name_->text().trimmed().toStdString();
    if (!tex::validGroupName(name)) {
        status_->setText(QStringLiteral("Name: letters, digits, _ and - with / for folders"));
        return "invalid name";
    }
    reparse();
    if (!valid_) {
        status_->setText(QStringLiteral("Fix the errors first"));
        return "the group has errors";
    }
    const nlohmann::json value = nlohmann::json::parse(text_->toPlainText().toStdString(), nullptr, false);
    if (value.is_discarded()) {
        status_->setText(QStringLiteral("The text is not valid JSON"));
        return "not valid JSON";
    }
    const std::string err = win_.texGroupWrite(name, value);
    if (!err.empty()) {
        status_->setText(QString::fromStdString("Not saved: " + err));
        return err;
    }
    if (original_.empty()) {
        original_ = name;
        name_->setReadOnly(true);
        setWindowTitle(QString::fromStdString("Texture group: " + name));
    }
    status_->setText(QString::fromStdString("Saved " + name));
    emit saved(QString::fromStdString(name));
    return {};
}

std::string TexGroupEditor::saveAndApply() {
    const std::string err = save();
    if (!err.empty()) return err;
    const bool done = win_.applyTexGroup(original_);
    status_->setText(done ? QString::fromStdString("Saved and applied " + original_) : QString::fromStdString("Saved " + original_ + ". Select a brush or face to apply it"));
    return {};
}

QWidget* TexGroupEditor::part(const QString& name) const { return findChild<QWidget*>(name); }
QString TexGroupEditor::status() const { return status_->text(); }

} // namespace native::ui
