// ui/tileset_editor -- see tileset_editor.h.
#include "ui/tileset_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

#include "core/texplan.h"
#include "tools/texturing/group_tools.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"
#include "ui/texture_browser_panel.h"

namespace native::ui {

namespace tex = native::tools::tex;
namespace tp = core::texplan;
using ojson = nlohmann::ordered_json;

namespace {

const char* kLaneTitle[3] = {"Top row", "Middle rows", "Bottom row"};
const char* kLaneHint[3] = {"Rooftop or cap: always above everything else", "Repeats upward to fill the height of the wall", "Ground floor: always at the bottom"};
const char* kFrameLabel[9] = {"Top left corner", "Top edge (repeats)", "Top right corner", "Left edge (repeats)", "Middle (repeats)", "Right edge (repeats)", "Bottom left corner", "Bottom edge (repeats)", "Bottom right corner"};
const char* kPlaceLabel[5] = {"Anywhere", "Left end only", "Right end only", "Middle only", "Exactly once per row"};

QString leaf(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return QString::fromStdString(s == std::string::npos ? path : path.substr(s + 1));
}

QString weightWord(double w) {
    if (std::abs(w - tex::kWeightRare) < 1e-6) return QStringLiteral("rare");
    if (std::abs(w - tex::kWeightSometimes) < 1e-6) return QStringLiteral("sometimes");
    if (std::abs(w - tex::kWeightOften) < 1e-6) return QStringLiteral("often");
    return QStringLiteral("x%1").arg(w, 0, 'g', 3);
}

// Several textures at once from the list of the Textures panel.
std::vector<std::string> pickMany(QWidget* parent, const std::vector<std::string>& all) {
    QDialog d(parent);
    d.setWindowTitle(QStringLiteral("Add textures to the set"));
    auto* v = new QVBoxLayout(&d);
    auto* search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("Search textures"));
    auto* list = new QListWidget;
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(search);
    v->addWidget(list, 1);
    v->addWidget(new QLabel(QStringLiteral("Select several. Names like top / bottom / roof / door / window sort them into rows.")));
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    const auto fill = [&] {
        list->clear();
        const QString q = search->text().toLower();
        int n = 0;
        for (const std::string& p : all) {
            const QString s = QString::fromStdString(p);
            if (!q.isEmpty() && !s.toLower().contains(q)) continue;
            list->addItem(s);
            if (++n >= 1500) break;
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, &d, fill);
    fill();
    d.resize(520, 560);
    std::vector<std::string> out;
    if (d.exec() == QDialog::Accepted)
        for (QListWidgetItem* it : list->selectedItems()) out.push_back(it->text().toStdString());
    return out;
}

} // namespace

TileSetEditor::TileSetEditor(MainWindow& window, const std::string& name, tex::KitKind kind) : QDialog(&window), win_(window), original_(name) {
    setObjectName(QStringLiteral("TileSetEditor"));
    kit_ = kind == tex::KitKind::Frame ? tex::emptyFrameKit() : tex::emptyWallKit();
    QString note;
    if (!name.empty()) {
        if (const auto raw = win_.texGroupRead(name)) {
            if (raw->contains("name") && (*raw)["name"].is_string()) title_ = (*raw)["name"].get<std::string>();
            if (raw->contains("description") && (*raw)["description"].is_string()) description_ = (*raw)["description"].get<std::string>();
            std::vector<std::string> w;
            if (raw->contains("kit")) {
                if (auto k = tex::kitFromJson((*raw)["kit"], w)) kit_ = std::move(*k);
            } else {
                note = QStringLiteral("'%1' is not a simple set. Open it in the Advanced tab.").arg(QString::fromStdString(name));
            }
            kitWarnings_ = std::move(w);
        } else {
            note = QStringLiteral("Could not read '%1'").arg(QString::fromStdString(name));
        }
    }
    setWindowTitle(name.empty() ? (kit_.kind == tex::KitKind::Frame ? QStringLiteral("New 9-slice frame") : QStringLiteral("New wall set")) : QString::fromStdString("Texture set: " + name));
    resize(1180, 720);
    build();
    titleEdit_->setText(QString::fromStdString(title_));
    refresh();
    if (!note.isEmpty()) status_->setText(note);
}

TileSetEditor::~TileSetEditor() = default;

void TileSetEditor::build() {
    auto* root = new QVBoxLayout(this);
    auto* bar = new QHBoxLayout;
    name_ = new QLineEdit(QString::fromStdString(original_));
    name_->setObjectName(QStringLiteral("name"));
    name_->setPlaceholderText(QStringLiteral("folder/name"));
    name_->setMinimumWidth(220);
    name_->setReadOnly(!original_.empty());
    titleEdit_ = new QLineEdit;
    titleEdit_->setObjectName(QStringLiteral("title"));
    titleEdit_->setPlaceholderText(QStringLiteral("Title (optional)"));
    auto* saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setObjectName(QStringLiteral("btn:save"));
    auto* applyBtn = new QPushButton(QStringLiteral("Save and apply to selection"));
    applyBtn->setObjectName(QStringLiteral("btn:apply"));
    auto* addMany = new QPushButton(QStringLiteral("Add textures..."));
    addMany->setObjectName(QStringLiteral("btn:addTextures"));
    addMany->setToolTip(QStringLiteral("Choose several textures: they sort themselves into rows by name (top, bottom, roof, door, window)"));
    addMany->setVisible(kit_.kind == tex::KitKind::Wall);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("status"));
    for (QWidget* w : std::initializer_list<QWidget*>{name_, titleEdit_, addMany, saveBtn, applyBtn}) bar->addWidget(w);
    bar->addWidget(status_, 1);
    root->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal);
    // left: the board, the inspector, the options
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lanesBox_ = new QWidget;
    lanesBox_->setObjectName(QStringLiteral("lanes"));
    auto* lanesLayout = new QVBoxLayout(lanesBox_);
    lanesLayout->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 3; ++i) {
        const tex::Lane lane = tex::kLanes[i];
        auto* g = new QGroupBox(QString::fromLatin1(kLaneTitle[i]));
        g->setToolTip(QString::fromLatin1(kLaneHint[i]));
        auto* gv = new QVBoxLayout(g);
        auto* head = new QHBoxLayout;
        laneRows_[i] = new QLabel;
        laneRows_[i]->setObjectName(QStringLiteral("rows:") + QString::fromLatin1(tex::laneName(lane)));
        auto* minus = new QPushButton(QStringLiteral("-"));
        minus->setObjectName(QStringLiteral("btn:rows-:") + QString::fromLatin1(tex::laneName(lane)));
        minus->setToolTip(QStringLiteral("One tile row less per repeat"));
        auto* plus = new QPushButton(QStringLiteral("+"));
        plus->setObjectName(QStringLiteral("btn:rows+:") + QString::fromLatin1(tex::laneName(lane)));
        plus->setToolTip(QStringLiteral("One more tile row: pieces can be two or more storeys tall"));
        auto* add = new QPushButton(QStringLiteral("+ Add (active texture)"));
        add->setObjectName(QStringLiteral("btn:add:") + QString::fromLatin1(tex::laneName(lane)));
        head->addWidget(new QLabel(QString::fromLatin1(kLaneHint[i])), 1);
        head->addWidget(laneRows_[i]);
        head->addWidget(minus);
        head->addWidget(plus);
        head->addWidget(add);
        gv->addLayout(head);
        laneLists_[i] = new QListWidget;
        laneLists_[i]->setObjectName(QStringLiteral("lane:") + QString::fromLatin1(tex::laneName(lane)));
        laneLists_[i]->setViewMode(QListView::IconMode);
        laneLists_[i]->setIconSize(QSize(40, 40));
        laneLists_[i]->setResizeMode(QListView::Adjust);
        laneLists_[i]->setMovement(QListView::Static);
        laneLists_[i]->setWrapping(true);
        laneLists_[i]->setMaximumHeight(112);
        laneLists_[i]->setSelectionMode(QAbstractItemView::SingleSelection);
        gv->addWidget(laneLists_[i]);
        lanesLayout->addWidget(g);
        connect(minus, &QPushButton::clicked, this, [this, lane] {
            tex::setLaneRows(kit_, lane, kit_.lane(lane).rows - 1);
            refresh();
        });
        connect(plus, &QPushButton::clicked, this, [this, lane] {
            tex::setLaneRows(kit_, lane, kit_.lane(lane).rows + 1);
            refresh();
        });
        connect(add, &QPushButton::clicked, this, [this, lane] { addPiece(lane, win_.activeTexture()); });
        connect(laneLists_[i], &QListWidget::currentRowChanged, this, [this, lane](int row) {
            if (!building_ && row >= 0) selectPiece(lane, row);
        });
    }
    lv->addWidget(lanesBox_);
    frameBox_ = new QWidget;
    frameBox_->setObjectName(QStringLiteral("frame"));
    new QGridLayout(frameBox_);
    lv->addWidget(frameBox_);

    // inspector
    inspectorBox_ = new QWidget;
    auto* iv = new QVBoxLayout(inspectorBox_);
    iv->setContentsMargins(0, 0, 0, 0);
    pieceBox_ = new QWidget;
    pieceBox_->setObjectName(QStringLiteral("piece"));
    auto* pv = new QVBoxLayout(pieceBox_);
    pv->setContentsMargins(0, 0, 0, 0);
    auto* size = new QHBoxLayout;
    wide_ = new QSpinBox;
    wide_->setObjectName(QStringLiteral("wide"));
    wide_->setRange(1, tex::kMaxPiece);
    wide_->setPrefix(QStringLiteral("wide "));
    tall_ = new QSpinBox;
    tall_->setObjectName(QStringLiteral("tall"));
    tall_->setRange(1, tex::kMaxPiece);
    tall_->setPrefix(QStringLiteral("tall "));
    tall_->setToolTip(QStringLiteral("Changes the whole row"));
    size->addWidget(new QLabel(QStringLiteral("<b>Selected piece</b>")));
    size->addWidget(wide_);
    size->addWidget(tall_);
    size->addStretch(1);
    pv->addLayout(size);
    cellsBox_ = new QWidget;
    cellsBox_->setObjectName(QStringLiteral("cells"));
    new QGridLayout(cellsBox_);
    pv->addWidget(cellsBox_);
    auto* props = new QFormLayout;
    weight_ = new QComboBox;
    weight_->setObjectName(QStringLiteral("weight"));
    weight_->addItem(QStringLiteral("Rare"), tex::kWeightRare);
    weight_->addItem(QStringLiteral("Sometimes"), tex::kWeightSometimes);
    weight_->addItem(QStringLiteral("Often"), tex::kWeightOften);
    place_ = new QComboBox;
    place_->setObjectName(QStringLiteral("place"));
    for (int i = 0; i < 5; ++i) place_->addItem(QString::fromLatin1(kPlaceLabel[i]), i);
    match_ = new QLineEdit;
    match_->setObjectName(QStringLiteral("match"));
    match_->setPlaceholderText(QStringLiteral("any"));
    match_->setMaxLength(32);
    match_->setToolTip(QStringLiteral("Pieces with different names never sit directly on each other. Pieces without a name sit on anything."));
    laneBox_ = new QComboBox;
    laneBox_->setObjectName(QStringLiteral("lane"));
    for (int i = 0; i < 3; ++i) laneBox_->addItem(QString::fromLatin1(kLaneTitle[i]), i);
    props->addRow(QStringLiteral("How often"), weight_);
    props->addRow(QStringLiteral("Where"), place_);
    props->addRow(QStringLiteral("Lines up with"), match_);
    props->addRow(QStringLiteral("Row"), laneBox_);
    pv->addLayout(props);
    auto* pieceButtons = new QHBoxLayout;
    auto* dup = new QPushButton(QStringLiteral("Duplicate"));
    dup->setObjectName(QStringLiteral("btn:duplicate"));
    auto* del = new QPushButton(QStringLiteral("Delete"));
    del->setObjectName(QStringLiteral("btn:delete"));
    pieceButtons->addWidget(dup);
    pieceButtons->addWidget(del);
    pieceButtons->addStretch(1);
    pv->addLayout(pieceButtons);
    iv->addWidget(pieceBox_);
    auto* tr = new QHBoxLayout;
    texture_ = new QLineEdit;
    texture_->setObjectName(QStringLiteral("texture"));
    texture_->setPlaceholderText(QStringLiteral("texture of the selected tile"));
    auto* set = new QPushButton(QStringLiteral("Set"));
    set->setObjectName(QStringLiteral("btn:set"));
    auto* use = new QPushButton(QStringLiteral("Use active texture"));
    use->setObjectName(QStringLiteral("btn:useActive"));
    auto* clear = new QPushButton(QStringLiteral("Clear"));
    clear->setObjectName(QStringLiteral("btn:clear"));
    tr->addWidget(texture_, 1);
    tr->addWidget(set);
    tr->addWidget(use);
    tr->addWidget(clear);
    iv->addLayout(tr);
    lv->addWidget(inspectorBox_);

    // options
    auto* opts = new QHBoxLayout;
    tileAuto_ = new QCheckBox(QStringLiteral("Tile size from the first texture"));
    tileAuto_->setObjectName(QStringLiteral("tileAuto"));
    tileW_ = new QDoubleSpinBox;
    tileW_->setObjectName(QStringLiteral("tileW"));
    tileH_ = new QDoubleSpinBox;
    tileH_->setObjectName(QStringLiteral("tileH"));
    for (QDoubleSpinBox* s : {tileW_, tileH_}) {
        s->setRange(0.1, 1000);
        s->setDecimals(2);
        s->setSingleStep(0.1);
        s->setValue(1);
        s->setSuffix(QStringLiteral(" m"));
    }
    maxStretch_ = new QSpinBox;
    maxStretch_->setObjectName(QStringLiteral("maxStretch"));
    maxStretch_->setRange(0, 500);
    maxStretch_->setSingleStep(5);
    maxStretch_->setPrefix(QStringLiteral("max stretch "));
    maxStretch_->setSuffix(QStringLiteral(" %"));
    maxStretch_->setToolTip(QStringLiteral("How much a tile may be scaled to make a whole number fit. 0 = no limit."));
    avoid_ = new QCheckBox(QStringLiteral("No same piece twice in a row"));
    avoid_->setObjectName(QStringLiteral("avoidRepeat"));
    for (QWidget* w : std::initializer_list<QWidget*>{tileAuto_, tileW_, tileH_, maxStretch_, avoid_}) opts->addWidget(w);
    lv->addLayout(opts);
    lv->addStretch(1);
    split->addWidget(left);

    // right: preview
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    auto* controls = new QHBoxLayout;
    w_ = new QDoubleSpinBox;
    w_->setObjectName(QStringLiteral("width"));
    h_ = new QDoubleSpinBox;
    h_->setObjectName(QStringLiteral("height"));
    for (QDoubleSpinBox* s : {w_, h_}) {
        s->setRange(0.5, 200);
        s->setDecimals(2);
        s->setSingleStep(0.5);
        s->setSuffix(QStringLiteral(" m"));
    }
    w_->setValue(9);
    h_->setValue(10);
    w_->setPrefix(QStringLiteral("width "));
    h_->setPrefix(QStringLiteral("height "));
    seed_ = new QSpinBox;
    seed_->setObjectName(QStringLiteral("seed"));
    seed_->setRange(0, 999999999);
    seed_->setValue(1);
    seed_->setPrefix(QStringLiteral("seed "));
    auto* reroll = new QPushButton(QStringLiteral("Re-roll"));
    reroll->setObjectName(QStringLiteral("btn:reroll"));
    for (QWidget* w : std::initializer_list<QWidget*>{w_, h_, seed_, reroll}) controls->addWidget(w);
    controls->addStretch(1);
    rv->addLayout(controls);
    preview_ = new GroupPreviewCanvas;
    preview_->setLabels(false);
    preview_->setSources([this](const std::string& p) { return win_.textureInfo() ? win_.textureInfo()(p) : nullptr; }, [this](const std::string& p) { return win_.loadTextureImage(p); });
    rv->addWidget(preview_, 1);
    problems_ = new QListWidget;
    problems_->setObjectName(QStringLiteral("problems"));
    problems_->setMaximumHeight(120);
    problems_->setSelectionMode(QAbstractItemView::NoSelection);
    rv->addWidget(problems_);
    split->addWidget(right);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 4);
    root->addWidget(split, 1);

    const auto view = [this] { redraw(); };
    connect(w_, &QDoubleSpinBox::valueChanged, this, view);
    connect(h_, &QDoubleSpinBox::valueChanged, this, view);
    connect(seed_, &QSpinBox::valueChanged, this, view);
    connect(reroll, &QPushButton::clicked, this, [this] { seed_->setValue(static_cast<int>(QRandomGenerator::global()->bounded(1000000))); });
    connect(titleEdit_, &QLineEdit::textChanged, this, [this](const QString& t) { title_ = t.toStdString(); });
    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(applyBtn, &QPushButton::clicked, this, [this] { saveAndApply(); });
    connect(addMany, &QPushButton::clicked, this, [this] {
        const std::vector<std::string> all = win_.textureBrowser() ? win_.textureBrowser()->textures() : std::vector<std::string>{};
        const auto chosen = pickMany(this, all);
        if (!chosen.empty()) addTextures(chosen);
    });
    connect(set, &QPushButton::clicked, this, [this] { setTargetTexture(texture_->text().trimmed().toStdString()); });
    connect(use, &QPushButton::clicked, this, [this] { setTargetTexture(win_.activeTexture()); });
    connect(clear, &QPushButton::clicked, this, [this] { setTargetTexture({}); });
    connect(dup, &QPushButton::clicked, this, [this] { duplicateSelected(); });
    connect(del, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(wide_, &QSpinBox::valueChanged, this, [this](int v) {
        if (building_) return;
        if (auto* p = selectedPiece()) {
            tex::fitPieceCols(*p, v);
            refresh();
        }
    });
    connect(tall_, &QSpinBox::valueChanged, this, [this](int v) {
        if (building_ || !selLane_) return;
        tex::setLaneRows(kit_, *selLane_, v);
        refresh();
    });
    connect(weight_, &QComboBox::activated, this, [this](int i) {
        if (auto* p = selectedPiece(); p && i >= 0) {
            p->weight = weight_->itemData(i).toDouble();
            refresh(false);
            rebuildLanes();
        }
    });
    connect(place_, &QComboBox::activated, this, [this](int i) {
        if (auto* p = selectedPiece(); p && i >= 0) {
            p->place = static_cast<tex::Place>(place_->itemData(i).toInt());
            refresh(false);
            rebuildLanes();
        }
    });
    connect(match_, &QLineEdit::editingFinished, this, [this] {
        if (building_) return;
        if (auto* p = selectedPiece()) {
            const QString m = match_->text().trimmed();
            static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9 _\\-]{1,32}$"));
            p->match = re.match(m).hasMatch() ? std::optional<std::string>(m.toStdString()) : std::nullopt;
            refresh(false);
            rebuildLanes();
        }
    });
    connect(laneBox_, &QComboBox::activated, this, [this](int i) {
        if (i >= 0) moveSelectedTo(tex::kLanes[i]);
    });
    connect(tileAuto_, &QCheckBox::toggled, this, [this](bool on) {
        if (building_) return;
        kit_.tile = on ? std::nullopt : std::optional<std::array<double, 2>>(std::array<double, 2>{tileW_->value(), tileH_->value()});
        refresh(false);
    });
    const auto tileChanged = [this] {
        if (building_ || tileAuto_->isChecked()) return;
        kit_.tile = std::array<double, 2>{tileW_->value(), tileH_->value()};
        refresh(false);
    };
    connect(tileW_, &QDoubleSpinBox::valueChanged, this, tileChanged);
    connect(tileH_, &QDoubleSpinBox::valueChanged, this, tileChanged);
    connect(maxStretch_, &QSpinBox::valueChanged, this, [this](int v) {
        if (building_) return;
        kit_.maxStretch = v / 100.0;
        refresh(false);
    });
    connect(avoid_, &QCheckBox::toggled, this, [this](bool on) {
        if (building_) return;
        kit_.avoidRepeat = on;
        refresh(false);
    });
}

void TileSetEditor::setKit(const tex::Kit& kit) {
    kit_ = kit;
    selLane_.reset();
    selIndex_ = -1;
    selSlot_ = -1;
    refresh();
}

void TileSetEditor::setName(const QString& name) {
    if (original_.empty()) name_->setText(name);
}

QString TileSetEditor::status() const { return status_->text(); }
QWidget* TileSetEditor::part(const QString& name) const { return findChild<QWidget*>(name); }

tex::KitPiece* TileSetEditor::selectedPiece() {
    if (kit_.kind != tex::KitKind::Wall || !selLane_) return nullptr;
    auto& v = kit_.lane(*selLane_).pieces;
    return selIndex_ >= 0 && selIndex_ < static_cast<int>(v.size()) ? &v[static_cast<size_t>(selIndex_)] : nullptr;
}

void TileSetEditor::selectPiece(tex::Lane lane, int index) {
    selLane_ = lane;
    selIndex_ = index;
    selRow_ = selCol_ = 0;
    if (!selectedPiece()) {
        selLane_.reset();
        selIndex_ = -1;
    }
    refresh();
}

void TileSetEditor::selectCell(int row, int col) {
    if (const auto* p = selectedPiece())
        if (row >= 0 && row < p->rows() && col >= 0 && col < p->cols()) {
            selRow_ = row;
            selCol_ = col;
        }
    rebuildInspector();
}

void TileSetEditor::selectSlot(int slot) {
    selSlot_ = slot >= 0 && slot < 9 ? slot : -1;
    rebuildFrame();
    rebuildInspector();
}

void TileSetEditor::setTargetTexture(const std::string& path) {
    const std::optional<std::string> t = path.empty() ? std::nullopt : std::optional<std::string>(path);
    if (t && !core::texplan::safeTexturePath(*t)) {
        status_->setText(QStringLiteral("Not a valid texture path"));
        return;
    }
    if (kit_.kind == tex::KitKind::Frame) {
        if (selSlot_ < 0) return;
        kit_.frame[static_cast<size_t>(selSlot_)] = t;
    } else if (selLane_) {
        if (!tex::setPieceCell(kit_, *selLane_, selIndex_, selRow_, selCol_, t)) return;
    } else {
        return;
    }
    refresh();
}

int TileSetEditor::addTextures(const std::vector<std::string>& paths) {
    if (kit_.kind != tex::KitKind::Wall) return 0;
    const int n = tex::addTextures(kit_, paths);
    status_->setText(QString::number(n) + QStringLiteral(" piece%1 added: check the rows, change them in the inspector").arg(n == 1 ? "" : "s"));
    refresh();
    return n;
}

int TileSetEditor::addPiece(tex::Lane lane, const std::string& texture) {
    if (kit_.kind != tex::KitKind::Wall) return -1;
    const std::optional<std::string> t = texture.empty() ? std::nullopt : std::optional<std::string>(texture);
    const int i = tex::addPiece(kit_, lane, t, tex::Place::Any, lane == tex::Lane::Middle ? tex::kWeightOften : tex::kWeightSometimes);
    if (i >= 0) {
        selLane_ = lane;
        selIndex_ = i;
        selRow_ = selCol_ = 0;
    }
    refresh();
    return i;
}

void TileSetEditor::moveSelectedTo(tex::Lane lane) {
    if (!selLane_ || selIndex_ < 0) return;
    const int at = tex::movePiece(kit_, *selLane_, selIndex_, lane);
    if (at < 0) return;
    selLane_ = lane;
    selIndex_ = at;
    selRow_ = selCol_ = 0;
    refresh();
}

void TileSetEditor::duplicateSelected() {
    if (!selLane_) return;
    const int i = tex::duplicatePiece(kit_, *selLane_, selIndex_);
    if (i >= 0) selIndex_ = i;
    refresh();
}

void TileSetEditor::deleteSelected() {
    if (!selLane_) return;
    tex::removePiece(kit_, *selLane_, selIndex_);
    selLane_.reset();
    selIndex_ = -1;
    refresh();
}

// ---- drawing the window -------------------------------------------------------------------------------------------------------------------------
void TileSetEditor::clearBox(QWidget* box) {
    QLayout* l = box->layout();
    while (QLayoutItem* it = l->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete it;
    }
}

QIcon TileSetEditor::iconOf(const tex::KitPiece& p) {
    constexpr int S = 40;
    QImage img(S, S, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x2a, 0x2a, 0x2a));
    QPainter g(&img);
    const int rows = std::max(1, p.rows()), cols = std::max(1, p.cols());
    const double cw = static_cast<double>(S) / cols, ch = static_cast<double>(S) / rows;
    for (int r = 0; r < p.rows(); ++r)
        for (int c = 0; c < p.cols(); ++c) {
            const QRectF cell(c * cw, r * ch, cw, ch);
            const auto& t = p.cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
            if (t) {
                auto it = images_.find(*t);
                if (it == images_.end()) it = images_.emplace(*t, win_.loadTextureImage(*t)).first;
                if (!it->second.isNull()) g.drawImage(cell, it->second);
                else g.fillRect(cell, QColor::fromHsl(static_cast<int>(std::hash<std::string>{}(*t) % 360), 64, 76));
            } else {
                g.setPen(QColor(0x99, 0x99, 0x99));
                g.drawRect(cell.adjusted(1.5, 1.5, -1.5, -1.5));
            }
        }
    return QIcon(QPixmap::fromImage(img));
}

void TileSetEditor::rebuildLanes() {
    const bool wall = kit_.kind == tex::KitKind::Wall;
    lanesBox_->setVisible(wall);
    frameBox_->setVisible(!wall);
    if (!wall) return;
    building_ = true;
    for (int i = 0; i < 3; ++i) {
        const tex::KitLane& lane = kit_.lanes[i];
        laneRows_[i]->setText(QStringLiteral("%1 tile row%2").arg(lane.rows).arg(lane.rows == 1 ? "" : "s"));
        QListWidget* list = laneLists_[i];
        list->clear();
        for (const tex::KitPiece& p : lane.pieces) {
            QStringList tags{weightWord(p.weight)};
            if (p.place != tex::Place::Any) tags << QString::fromLatin1(tex::placeName(p.place));
            if (p.match) tags << QStringLiteral("= ") + QString::fromStdString(*p.match);
            const auto first = p.cells.empty() || p.cells[0].empty() ? std::optional<std::string>() : p.cells[0][0];
            auto* item = new QListWidgetItem(iconOf(p), (first ? leaf(*first) : QStringLiteral("(empty)")) + QStringLiteral("\n") + tags.join(QStringLiteral(" · ")), list);
            item->setToolTip(QStringLiteral("%1 x %2 tiles").arg(p.cols()).arg(p.rows()));
        }
        if (selLane_ && static_cast<int>(*selLane_) == i && selIndex_ >= 0 && selIndex_ < list->count()) list->setCurrentRow(selIndex_);
    }
    building_ = false;
}

void TileSetEditor::rebuildFrame() {
    clearBox(frameBox_);
    if (kit_.kind != tex::KitKind::Frame) return;
    auto* grid = static_cast<QGridLayout*>(frameBox_->layout());
    for (int i = 0; i < 9; ++i) {
        const auto& t = kit_.frame[static_cast<size_t>(i)];
        auto* b = new QPushButton(QString::fromLatin1(kFrameLabel[i]) + QStringLiteral("\n") + (t ? leaf(*t) : QStringLiteral("(none)")));
        b->setObjectName(QStringLiteral("slot:") + QString::fromLatin1(tex::kFrameSlots[i]));
        b->setCheckable(true);
        b->setChecked(i == selSlot_);
        b->setMinimumHeight(56);
        if (t) {
            auto it = images_.find(*t);
            if (it == images_.end()) it = images_.emplace(*t, win_.loadTextureImage(*t)).first;
            if (!it->second.isNull()) {
                b->setIcon(QIcon(QPixmap::fromImage(it->second.scaled(40, 40))));
                b->setIconSize(QSize(40, 40));
            }
        }
        connect(b, &QPushButton::clicked, this, [this, i] { selectSlot(i); });
        grid->addWidget(b, i / 3, i % 3);
    }
}

void TileSetEditor::rebuildInspector() {
    building_ = true;
    const bool wall = kit_.kind == tex::KitKind::Wall;
    tex::KitPiece* p = selectedPiece();
    pieceBox_->setVisible(wall && p);
    clearBox(cellsBox_);
    std::optional<std::string> target;
    if (wall && p) {
        wide_->setValue(p->cols());
        tall_->setValue(p->rows());
        auto* grid = static_cast<QGridLayout*>(cellsBox_->layout());
        for (int r = 0; r < p->rows(); ++r)
            for (int c = 0; c < p->cols(); ++c) {
                const auto& t = p->cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
                auto* b = new QPushButton(t ? leaf(*t) : QStringLiteral("+"));
                b->setObjectName(QStringLiteral("cell:%1:%2").arg(r).arg(c));
                b->setCheckable(true);
                b->setChecked(r == selRow_ && c == selCol_);
                b->setMinimumSize(70, 44);
                if (t) {
                    auto it = images_.find(*t);
                    if (it == images_.end()) it = images_.emplace(*t, win_.loadTextureImage(*t)).first;
                    if (!it->second.isNull()) b->setIcon(QIcon(QPixmap::fromImage(it->second.scaled(32, 32))));
                }
                connect(b, &QPushButton::clicked, this, [this, r, c] { selectCell(r, c); });
                grid->addWidget(b, r, c);
            }
        // the weight box has the three words and, for another value, one more
        while (weight_->count() > 3) weight_->removeItem(3);
        int wi = -1;
        for (int i = 0; i < 3; ++i)
            if (std::abs(weight_->itemData(i).toDouble() - p->weight) < 1e-6) wi = i;
        if (wi < 0) {
            weight_->addItem(QStringLiteral("x%1").arg(p->weight, 0, 'g', 3), p->weight);
            wi = 3;
        }
        weight_->setCurrentIndex(wi);
        place_->setCurrentIndex(static_cast<int>(p->place));
        match_->setText(p->match ? QString::fromStdString(*p->match) : QString());
        laneBox_->setCurrentIndex(static_cast<int>(*selLane_));
        if (selRow_ < p->rows() && selCol_ < p->cols()) target = p->cells[static_cast<size_t>(selRow_)][static_cast<size_t>(selCol_)];
    } else if (!wall && selSlot_ >= 0) {
        target = kit_.frame[static_cast<size_t>(selSlot_)];
    }
    texture_->setText(target ? QString::fromStdString(*target) : QString());
    const bool haveTarget = (wall && p) || (!wall && selSlot_ >= 0);
    for (const char* n : {"texture", "btn:set", "btn:useActive", "btn:clear"})
        if (QWidget* w = part(QString::fromLatin1(n))) w->setEnabled(haveTarget);
    building_ = false;
}

void TileSetEditor::refresh(bool boards) {
    if (selLane_) {   // keep the selection inside the set
        auto& v = kit_.lane(*selLane_).pieces;
        if (selIndex_ < 0 || selIndex_ >= static_cast<int>(v.size())) {
            selLane_.reset();
            selIndex_ = -1;
        }
    }
    building_ = true;
    tileAuto_->setChecked(!kit_.tile);
    tileW_->setEnabled(kit_.tile.has_value());
    tileH_->setEnabled(kit_.tile.has_value());
    if (kit_.tile) {
        tileW_->setValue((*kit_.tile)[0]);
        tileH_->setValue((*kit_.tile)[1]);
    }
    maxStretch_->setValue(static_cast<int>(std::lround(kit_.maxStretch * 100)));
    avoid_->setChecked(kit_.avoidRepeat);
    avoid_->setVisible(kit_.kind == tex::KitKind::Wall);
    building_ = false;
    if (boards) {
        rebuildLanes();
        rebuildFrame();
    }
    rebuildInspector();
    redraw();
}

void TileSetEditor::redraw() {
    solveWarnings_.clear();
    warnings_.clear();
    valid_ = false;
    const ojson file = tex::tileSetFile(kit_, title_, description_);
    const tp::GroupParse parse = tp::parseGroup(file);
    warnings_ = parse.warnings;
    for (const std::string& e : parse.errors) warnings_.push_back(e);
    if (parse.group && parse.errors.empty() && !tex::kitTextures(kit_).empty()) {
        valid_ = true;
        tp::LayoutInput in;
        in.group = parse.group.get();
        in.w = w_->value();
        in.h = h_->value();
        in.seed = static_cast<uint32_t>(seed_->value());
        in.info = win_.textureInfo();
        tp::LayoutResult r = tp::solveLayout(in);
        solveWarnings_ = r.warnings;
        if (r.truncated) solveWarnings_.push_back("The layout was cut: it has too many parts");
        preview_->setLayout(std::move(r.cells), in.w, in.h);
        if (const auto info = win_.textureInfo())
            for (const std::string& t : tex::kitTextures(kit_))
                if (!info(t)) solveWarnings_.push_back("texture '" + t + "' was not found in the assets folder");
    } else {
        preview_->setLayout({}, w_->value(), h_->value());
    }
    showProblems();
}

void TileSetEditor::showProblems() {
    problems_->clear();
    for (const auto* list : {&kitWarnings_, &warnings_, &solveWarnings_})
        for (const std::string& m : *list) {
            auto* it = new QListWidgetItem(QString::fromStdString(m), problems_);
            it->setForeground(QColor(0xe8, 0xb3, 0x39));
        }
    if (tex::kitTextures(kit_).empty()) problems_->addItem(kit_.kind == tex::KitKind::Frame ? QStringLiteral("Choose the textures of the nine slots.") : QStringLiteral("Add textures to start: \"Add textures...\" sorts them into rows by name."));
    else if (problems_->count() == 0) problems_->addItem(QStringLiteral("No problems found."));
}

// ---- saving ----------------------------------------------------------------------------------------------------------------------------------
std::string TileSetEditor::save() {
    const std::string name = name_->text().trimmed().toStdString();
    if (!tex::validGroupName(name)) {
        status_->setText(QStringLiteral("Name: letters, digits, _ and - with / for folders"));
        return "invalid name";
    }
    redraw();
    if (!valid_) {
        status_->setText(QStringLiteral("The set is not usable yet"));
        return "the set is not usable";
    }
    const nlohmann::json value = nlohmann::json::parse(tex::tileSetFile(kit_, title_, description_).dump());
    const std::string err = win_.texGroupWrite(name, value);
    if (!err.empty()) {
        status_->setText(QString::fromStdString("Not saved: " + err));
        return err;
    }
    if (original_.empty()) {
        original_ = name;
        name_->setReadOnly(true);
        setWindowTitle(QString::fromStdString("Texture set: " + name));
    }
    status_->setText(QString::fromStdString("Saved " + name));
    emit saved(QString::fromStdString(name));
    return {};
}

std::string TileSetEditor::saveAndApply() {
    const std::string err = save();
    if (!err.empty()) return err;
    const bool done = win_.applyTexGroup(original_);
    status_->setText(done ? QString::fromStdString("Saved and applied " + original_) : QString::fromStdString("Saved " + original_ + ". Select a brush or face to apply it"));
    return {};
}

} // namespace native::ui
