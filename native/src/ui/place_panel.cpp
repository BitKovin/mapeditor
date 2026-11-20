// ui/place_panel -- see place_panel.h.
#include "ui/place_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "core/entdef.h"
#include "scene/point_box.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

struct Starter {
    const char* classname;
    const char* label;
    const char* sub;
};

// With no entity definitions: the built-in starter list (the Shell's ENTITIES / LIGHTS).
const Starter kEntities[] = {
    {"info_player_start", "Player Start", ""}, {"info_player_deathmatch", "Deathmatch Spawn", ""}, {"info_notnull", "Info Notnull", ""},
    {"target_position", "Target Position", ""}, {"misc_model", "Model", ""},
};
const Starter kLights[] = {
    {"light_point", "Point Light", "shines in every direction"}, {"light_spot", "Spot Light", "a cone along its arrow"},
    {"light_directional", "Directional Light", "the sun: parallel rays"}, {"light_rect", "Rect Light", "a width x height panel"},
    {"light_sky", "Sky Light", "the upper hemisphere"},
};

bool isModernLight(const std::string& name) {
    for (const Starter& l : kLights)
        if (name == l.classname) return true;
    return false;
}

bool isLightClass(const std::string& name) {
    std::string l = name;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.rfind("light", 0) == 0;
}

struct ShapeEntry {
    tools::ShapeId id;
    const char* label;
    const char* key;
};
const ShapeEntry kShapes[] = {
    {tools::ShapeId::Box, "Box", "B"},       {tools::ShapeId::Cylinder, "Cylinder", ""}, {tools::ShapeId::Wedge, "Wedge", ""},
    {tools::ShapeId::Arch, "Arch", ""},      {tools::ShapeId::Stairs, "Stairs", ""},     {tools::ShapeId::Sphere, "Sphere", ""},
};

enum ParamRow { RowSides, RowSteps, RowSegments, RowRings, RowSmooth, RowArchSegments, RowArchThickness, RowTurn };

} // namespace

PlacePanel::PlacePanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("placePanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("placeTabs"));
    layout->addWidget(tabs_);

    auto* shapes = new QWidget;
    buildShapes(shapes);
    tabs_->addTab(shapes, QStringLiteral("Shapes"));
    refreshTooltips();
    QObject::connect(&win_, &MainWindow::keymapChanged, this, [this] { refreshTooltips(); });

    auto* prefabPage = new QWidget;
    auto* pl = new QVBoxLayout(prefabPage);
    pl->setContentsMargins(0, 4, 0, 0);
    auto* top = new QHBoxLayout;
    prefabFilter_ = new QLineEdit;
    prefabFilter_->setObjectName(QStringLiteral("prefabFilter"));
    prefabFilter_->setPlaceholderText(QStringLiteral("Filter prefabs…"));
    auto* refresh = new QPushButton(QStringLiteral("↻"));
    refresh->setObjectName(QStringLiteral("prefabRescan"));
    refresh->setToolTip(QStringLiteral("Rescan the prefab folder"));
    refresh->setFixedWidth(28);
    top->addWidget(prefabFilter_, 1);
    top->addWidget(refresh);
    pl->addLayout(top);
    prefabs_ = new QListWidget;
    prefabs_->setObjectName(QStringLiteral("prefabList"));
    pl->addWidget(prefabs_, 1);
    prefabInfo_ = new QLabel;
    prefabInfo_->setWordWrap(true);
    prefabInfo_->setEnabled(false);
    pl->addWidget(prefabInfo_);
    tabs_->addTab(prefabPage, QStringLiteral("Prefabs"));

    auto* entityPage = new QWidget;
    auto* el = new QVBoxLayout(entityPage);
    el->setContentsMargins(0, 4, 0, 0);
    entityFilter_ = new QLineEdit;
    entityFilter_->setObjectName(QStringLiteral("entityFilter"));
    entityFilter_->setPlaceholderText(QStringLiteral("Filter entities…"));
    el->addWidget(entityFilter_);
    entities_ = new QListWidget;
    entities_->setObjectName(QStringLiteral("entityList"));
    el->addWidget(entities_, 1);
    tabs_->addTab(entityPage, QStringLiteral("Entities"));

    lights_ = new QListWidget;
    lights_->setObjectName(QStringLiteral("lightList"));
    tabs_->addTab(lights_, QStringLiteral("Lights"));

    // a click adds the thing where the camera looks
    QObject::connect(entities_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const std::string cls = it->data(Qt::UserRole).toString().toStdString();
        const auto kv = keyvalues_.find(cls);
        win_.placeEntity(cls, kv == keyvalues_.end() ? std::vector<std::pair<std::string, std::string>>{} : kv->second);
    });
    QObject::connect(lights_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { win_.placeEntity(it->data(Qt::UserRole).toString().toStdString()); });
    QObject::connect(prefabs_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { win_.placePrefab(it->data(Qt::UserRole).toString().toStdString()); });
    QObject::connect(entityFilter_, &QLineEdit::textChanged, this, [this](const QString& t) { fillEntityList(entities_, entityClasses_, t); });
    QObject::connect(prefabFilter_, &QLineEdit::textChanged, this, [this] { drawPrefabs(); });
    QObject::connect(refresh, &QPushButton::clicked, this, [this] { rescanPrefabs(); });
    QObject::connect(&win_, &MainWindow::entityDefinitionsChanged, this, [this] {
        rebuildClasses();
        rescanPrefabs();
    });
    QObject::connect(&win_, &MainWindow::toolChanged, this, [this] { syncShape(); });
    QTimer::singleShot(0, this, [this] {   // the window is still being built: the lists are filled when it is up
        rebuildClasses();
        rescanPrefabs();
        syncShape();
    });
}

PlacePanel::~PlacePanel() = default;

// ---- shapes --------------------------------------------------------------------------------------------------------------------------

void PlacePanel::refreshTooltips() {
    shapeTips_.clear();
    for (size_t i = 0; i < shapeButtons_.size(); ++i) {
        const std::string keys = win_.keymap().displayText("shape.set", {{"shape", tools::shapeName(kShapes[i].id)}});
        QString tip = QString::fromLatin1(kShapes[i].label);
        if (!keys.empty()) tip += QStringLiteral(" (") + QString::fromStdString(keys) + QLatin1Char(')');
        tip += QStringLiteral(": drag on a surface (the start snaps to the grid), release, move for height, click. Shift = equal sides, Esc = cancel");
        shapeButtons_[i]->setToolTip(tip);
        shapeTips_.push_back(tip);
    }
    if (turn_) {
        const std::string keys = win_.keymap().displayText("shape.turn");
        turn_->setToolTip(QStringLiteral("Which way the shape faces in the box you draw (quarter turns about its height); editable later")
                          + (keys.empty() ? QString() : QStringLiteral(" - turn while drawing: ") + QString::fromStdString(keys)));
    }
}

void PlacePanel::buildShapes(QWidget* page) {
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 4, 0, 0);
    auto* grid = new QGridLayout;
    auto* group = new QButtonGroup(page);
    group->setExclusive(true);
    int i = 0;
    for (const ShapeEntry& s : kShapes) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("shape:") + QString::fromLatin1(tools::shapeName(s.id)));
        b->setText(QString::fromLatin1(s.label));
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setMinimumHeight(34);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setToolTip(QString::fromLatin1(s.label) + QStringLiteral(": drag on a surface (the start snaps to the grid), release, move for height, click. Shift = equal sides, Esc = cancel"));
        group->addButton(b);
        grid->addWidget(b, i / 2, i % 2);
        shapeButtons_.push_back(b);
        const tools::ShapeId id = s.id;
        QObject::connect(b, &QToolButton::clicked, this, [this, id] {
            if (!syncing_) win_.setShape(id);
        });
        ++i;
    }
    v->addLayout(grid);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 8, 0, 0);
    const auto addRow = [&](ParamRow row, const QString& label, QWidget* w) {
        auto* holder = new QWidget;
        auto* h = new QHBoxLayout(holder);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(new QLabel(label));
        h->addWidget(w, 1);
        paramRows_[row] = holder;
        v->addWidget(holder);
    };
    (void)form;
    const auto spin = [&](ParamRow row, const char* name, const QString& label, int lo, int hi) {
        auto* s = new QSpinBox;
        s->setObjectName(QString::fromLatin1(name));
        s->setRange(lo, hi);
        s->setKeyboardTracking(false);
        addRow(row, label, s);
        QObject::connect(s, &QSpinBox::valueChanged, this, [this] { applyParams(); });
        return s;
    };
    sides_ = spin(RowSides, "param:sides", QStringLiteral("Sides"), 3, 64);
    steps_ = spin(RowSteps, "param:steps", QStringLiteral("Steps"), 2, 64);
    segments_ = spin(RowSegments, "param:segments", QStringLiteral("Segments"), 4, 32);
    rings_ = spin(RowRings, "param:rings", QStringLiteral("Rings"), 2, 16);
    smooth_ = new QCheckBox;
    smooth_->setObjectName(QStringLiteral("param:smooth"));
    smooth_->setToolTip(QStringLiteral("Mark the round edges smooth: the light bake and the export shade it round"));
    addRow(RowSmooth, QStringLiteral("Smooth"), smooth_);
    QObject::connect(smooth_, &QCheckBox::clicked, this, [this] { applyParams(); });
    archSegments_ = spin(RowArchSegments, "param:archSegments", QStringLiteral("Arch segments"), 3, 64);
    archThickness_ = new QDoubleSpinBox;
    archThickness_->setObjectName(QStringLiteral("param:archThickness"));
    archThickness_->setRange(0.05, 20);
    archThickness_->setSingleStep(0.05);
    archThickness_->setDecimals(2);
    archThickness_->setKeyboardTracking(false);
    addRow(RowArchThickness, QStringLiteral("Wall thickness (m)"), archThickness_);
    QObject::connect(archThickness_, &QDoubleSpinBox::valueChanged, this, [this] { applyParams(); });
    turn_ = new QComboBox;
    turn_->setObjectName(QStringLiteral("param:turn"));
    turn_->setToolTip(QStringLiteral("Which way the shape faces in the box you draw (quarter turns about its height); editable later"));
    for (int t = 0; t < 4; ++t) turn_->addItem(QString::number(t * 90) + QStringLiteral("°"), t);
    addRow(RowTurn, QStringLiteral("Orientation"), turn_);
    QObject::connect(turn_, &QComboBox::activated, this, [this](int) { applyParams(); });
    shapeHint_ = new QLabel;
    shapeHint_->setWordWrap(true);
    shapeHint_->setEnabled(false);
    v->addWidget(shapeHint_);
    v->addStretch(1);
}

void PlacePanel::syncShape() {
    syncing_ = true;
    const tools::ShapeParams& p = win_.shapeParams();
    const tools::ShapeId shape = win_.shape();
    for (size_t i = 0; i < shapeButtons_.size(); ++i) shapeButtons_[i]->setChecked(kShapes[i].id == shape && win_.tool() == MainWindow::Tool::Box);
    sides_->setValue(p.sides);
    steps_->setValue(p.steps);
    segments_->setValue(p.segments);
    rings_->setValue(p.rings);
    smooth_->setChecked(p.smooth);
    archSegments_->setValue(p.archSegments);
    archThickness_->setValue(p.archThickness);
    turn_->setCurrentIndex(std::clamp(p.turn, 0, 3));
    using S = tools::ShapeId;
    paramRows_[RowSides]->setVisible(shape == S::Cylinder);
    paramRows_[RowSteps]->setVisible(shape == S::Stairs);
    paramRows_[RowSegments]->setVisible(shape == S::Sphere);
    paramRows_[RowRings]->setVisible(shape == S::Sphere);
    paramRows_[RowSmooth]->setVisible(shape == S::Cylinder || shape == S::Sphere);
    paramRows_[RowArchSegments]->setVisible(shape == S::Arch);
    paramRows_[RowArchThickness]->setVisible(shape == S::Arch);
    paramRows_[RowTurn]->setVisible(shape != S::Box);
    shapeHint_->setText(shape == S::Box ? QString()
                        : QStringLiteral("%1%2").arg(shape == S::Stairs || shape == S::Arch ? QStringLiteral("Builds several convex brushes, so it can live in worldspawn. ") : QString(),
                                                     QStringLiteral("Its box is drawn like any brush: drag on a surface, release, move for the height, click.")));
    syncing_ = false;
}

void PlacePanel::applyParams() {
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

// ---- entities, lights, prefabs -------------------------------------------------------------------------------------------------------

void PlacePanel::rebuildClasses() {
    entityClasses_.clear();
    lightClasses_.clear();
    keyvalues_.clear();
    const core::EntityDefinitions* defs = win_.entityDefinitions();
    bool anyPoints = false;
    if (defs)
        for (const core::EntClass& c : defs->classes)
            if (c.kind == core::EntClass::Kind::Point) anyPoints = true;
    if (anyPoints) {
        for (const core::EntClass& c : defs->classes) {
            if (c.kind != core::EntClass::Kind::Point) continue;
            if (!isLightClass(c.name)) entityClasses_.push_back(c.name);
        }
    } else {
        for (const Starter& e : kEntities) entityClasses_.push_back(e.classname);
        keyvalues_["misc_model"] = {{"model", ""}};
    }
    for (const Starter& l : kLights) lightClasses_.push_back(l.classname);
    if (anyPoints)
        for (const core::EntClass& c : defs->classes)
            if (c.kind == core::EntClass::Kind::Point && isLightClass(c.name) && !isModernLight(c.name) && c.name != "light") lightClasses_.push_back(c.name);
    fillEntityList(entities_, entityClasses_, entityFilter_->text());
    fillEntityList(lights_, lightClasses_, QString());
}

void PlacePanel::fillEntityList(QListWidget* list, const std::vector<std::string>& classes, const QString& filter) {
    list->clear();
    const core::EntityDefinitions* defs = win_.entityDefinitions();
    const scene::BoxOf boxOf = scene::classBoxes(defs);
    const QString q = filter.trimmed().toLower();
    int shown = 0;
    for (const std::string& name : classes) {
        if (!q.isEmpty() && !qs(name).toLower().contains(q)) continue;
        if (++shown > 300) break;
        QString label = qs(name);
        QString tip = label;
        for (const Starter& s : kEntities)
            if (name == s.classname && !(defs && defs->find(name))) label = QString::fromLatin1(s.label);
        for (const Starter& s : kLights)
            if (name == s.classname) {
                label = QString::fromLatin1(s.label);
                tip = qs(name) + QStringLiteral(": ") + QString::fromLatin1(s.sub);
            }
        const core::EntClass* def = defs ? defs->find(name) : nullptr;
        if (def && !def->description.empty() && !tip.contains(QStringLiteral(": "))) tip += QStringLiteral("\n") + qs(def->description.substr(0, def->description.find('\n')));
        const scene::Box b = boxOf(name);
        const double w = b.max[0] - b.min[0], h = b.max[1] - b.min[1], d = b.max[2] - b.min[2];
        auto* it = new QListWidgetItem(label + QStringLiteral("   ") + QString::number(w, 'g', 3) + QStringLiteral(" x ") + QString::number(d, 'g', 3) + QStringLiteral(" x ") + QString::number(h, 'g', 3) + QStringLiteral(" m"));
        it->setData(Qt::UserRole, qs(name));
        it->setToolTip(tip + QStringLiteral("\nBox %1 x %2 x %3 m (width x depth x height)\nClick to add").arg(w, 0, 'g', 3).arg(d, 0, 'g', 3).arg(h, 0, 'g', 3));
        QColor color(255, 217, 64);
        if (def) color = QColor::fromRgbF(def->color[0], def->color[1], def->color[2]);
        QPixmap swatch(14, 14);
        swatch.fill(color);
        it->setIcon(QIcon(swatch));
        list->addItem(it);
    }
}

void PlacePanel::rescanPrefabs() {
    prefabFiles_.clear();
    const std::string root = win_.assetsRoot();
    const std::string prefabRoot = win_.prefabRoot();
    if (!root.empty()) {
        const QString base = QString::fromStdString(root) + QStringLiteral("/") + qs(prefabRoot);
        QDirIterator it(base, {QStringLiteral("*.map.json")}, QDir::Files, QDirIterator::Subdirectories);
        const QString rootDir = QString::fromStdString(root);
        while (it.hasNext() && prefabFiles_.size() < 5000) {
            const QString abs = it.next();
            QString rel = abs.mid(rootDir.size());
            while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
            prefabFiles_.push_back(rel.toStdString());
        }
        std::sort(prefabFiles_.begin(), prefabFiles_.end());
    }
    drawPrefabs();
}

void PlacePanel::drawPrefabs() {
    prefabs_->clear();
    const QString q = prefabFilter_->text().trimmed().toLower();
    std::string root = win_.prefabRoot();
    while (!root.empty() && root.back() == '/') root.pop_back();
    for (const std::string& f : prefabFiles_) {
        if (!q.isEmpty() && !qs(f).toLower().contains(q)) continue;
        std::string label = !root.empty() && f.rfind(root + "/", 0) == 0 ? f.substr(root.size() + 1) : f;
        if (label.size() > 9 && label.compare(label.size() - 9, 9, ".map.json") == 0) label.resize(label.size() - 9);
        auto* it = new QListWidgetItem(qs(label));
        it->setData(Qt::UserRole, qs(f));
        it->setToolTip(qs(f) + QStringLiteral("\nClick to add an instance"));
        prefabs_->addItem(it);
    }
    prefabInfo_->setText(prefabFiles_.empty() ? QStringLiteral("No *.map.json files under '%1'.").arg(qs(root)) : QString());
}

} // namespace native::ui
