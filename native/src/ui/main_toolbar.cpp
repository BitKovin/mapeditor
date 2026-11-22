// ui/main_toolbar -- see main_toolbar.h.
#include "ui/main_toolbar.h"

#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QSizePolicy>
#include <QToolButton>

#include <cmath>

#include "app/actions/registry.h"
#include "render/viewport_window.h"
#include "tools/shapes.h"
#include "tools/status_text.h"
#include "ui/app_actions.h"
#include "ui/icons.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString titled(const std::string& name) {
    QString t = qs(name);
    if (!t.isEmpty()) t[0] = t[0].toUpper();
    return t;
}

} // namespace

MainToolBar::MainToolBar(MainWindow& window) : QToolBar(QStringLiteral("Main"), &window), win_(window) {
    setObjectName(QStringLiteral("mainToolbar"));
    setMovable(false);
    setFloatable(false);
    setIconSize(QSize(18, 18));
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setContextMenuPolicy(Qt::PreventContextMenu);

    using nlohmann::json;
    // file and history
    addRegistry("file.save", nullptr, "save");
    addRegistry("edit.undo", nullptr, "undo");
    addRegistry("edit.redo", nullptr, "redo");
    addSeparator();
    // tools: Select, Shape (a split button: the arrow lists the solids), Clipper, Decal, Measure
    addRegistry("tool.set", json{{"tool", "select"}}, "select");
    shapeAction_ = addRegistry("tool.set", json{{"tool", "box"}}, "box", QStringLiteral("Box"));
    if (shapeAction_) {
        shapeButton_ = qobject_cast<QToolButton*>(widgetForAction(shapeAction_));
        if (shapeButton_) {
            auto* menu = new QMenu(shapeButton_);
            menu->setObjectName(QStringLiteral("mainToolbar:shapes"));
            if (const actions::ActionDef* def = win_.registry().find("shape.set"))
                for (const actions::MenuVariant& v : def->variants) {
                    QAction* a = menu->addAction(qs(v.label));
                    const json params = v.params;
                    connect(a, &QAction::triggered, this, [this, params] {
                        win_.runAction("shape.set", params);
                        refresh();
                    });
                }
            shapeButton_->setMenu(menu);
            shapeButton_->setPopupMode(QToolButton::MenuButtonPopup);
            shapeButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
    }
    addRegistry("brush.clipper", nullptr, "clipper");
    addRegistry("tool.set", json{{"tool", "decal"}}, "decal");
    addRegistry("view.measure", nullptr, "measure");
    addSeparator();
    // edit mode: Object | Mesh, and the element kinds that only exist in mesh mode
    object_ = new QAction(icon("object"), QStringLiteral("Object"), this);
    object_->setCheckable(true);
    object_->setObjectName(QStringLiteral("mainToolbar:object"));
    connect(object_, &QAction::triggered, this, [this] {
        win_.leaveMeshMode();
        refresh();
    });
    addAction(object_);
    mesh_ = new QAction(icon("mesh"), QStringLiteral("Mesh"), this);
    mesh_->setCheckable(true);
    mesh_->setObjectName(QStringLiteral("mainToolbar:mesh"));
    connect(mesh_, &QAction::triggered, this, [this] {
        win_.enterMeshMode();
        refresh();
    });
    addAction(mesh_);
    for (QAction* a : {object_, mesh_})
        if (auto* b = qobject_cast<QToolButton*>(widgetForAction(a))) b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (const char* e : {"vertex", "edge", "face"})
        if (QAction* a = addRegistry("mesh.element", json{{"element", e}}, e)) elements_.push_back(a);
    addSeparator();
    // gizmo
    for (const char* m : {"move", "rotate", "scale"}) addRegistry("gizmo.mode", json{{"mode", m}}, m);
    addSeparator();
    // snapping: grid size, snap on / off, rotation step
    auto* gridLabel = new QLabel(this);
    gridLabel->setPixmap(icon("grid").pixmap(16, 16));
    gridLabel->setToolTip(QStringLiteral("Grid size of the active viewport ([ and ] change it)"));
    gridLabel->setContentsMargins(4, 0, 2, 0);
    addWidget(gridLabel);
    grid_ = new QComboBox(this);
    grid_->setObjectName(QStringLiteral("mainToolbar:grid"));
    grid_->setToolTip(QStringLiteral("Grid size of the active viewport ([ and ] change it)"));
    buildCombo(grid_, render::gridSizes(), QStringLiteral(" m"));
    addWidget(grid_);
    connect(grid_, &QComboBox::activated, this, [this](int i) {
        if (syncing_ || i < 0) return;
        win_.runAction("snap.grid_size", json{{"value", grid_->itemData(i).toDouble()}});
        refresh();
    });
    addRegistry("snap.toggle", nullptr, "snap");
    rotation_ = new QComboBox(this);
    rotation_->setObjectName(QStringLiteral("mainToolbar:rotation"));
    rotation_->setToolTip(QStringLiteral("Rotation step of the active viewport"));
    buildCombo(rotation_, render::rotSnaps(), QStringLiteral("\xC2\xB0"));
    addWidget(rotation_);
    connect(rotation_, &QComboBox::activated, this, [this](int i) {
        if (syncing_ || i < 0) return;
        win_.runAction("snap.rotation", json{{"value", rotation_->itemData(i).toDouble()}});
        refresh();
    });
    addSeparator();
    addRegistry("texture.uv_lock", nullptr, "uvlock");
    addSeparator();
    // viewport layout
    addRegistry("view.layout", json{{"count", 1}}, "layout1");
    addRegistry("view.layout", json{{"count", 2}}, "layout2");
    addRegistry("view.layout", json{{"count", 4}}, "layout4");
    // the right end: the command palette and Play
    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(spacer);
    search_ = new QToolButton(this);
    search_->setObjectName(QStringLiteral("mainToolbar:search"));
    search_->setIcon(icon("search"));
    search_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    search_->setCursor(Qt::PointingHandCursor);
    addWidget(search_);
    connect(search_, &QToolButton::clicked, this, [this] { win_.runAction("view.command_palette"); });
    addRegistry("file.play", nullptr, "play", QStringLiteral("Play"));
    if (QAction* play = find("file.play"))
        if (auto* b = qobject_cast<QToolButton*>(widgetForAction(play))) b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    for (const auto& signal : {&MainWindow::toolChanged, &MainWindow::viewportsChanged, &MainWindow::keymapChanged, &MainWindow::documentChanged, &MainWindow::selectionChanged,
                               &MainWindow::editorPrefsChanged, &MainWindow::transformChanged})
        connect(&win_, signal, this, [this] { refresh(); });
    refresh();
}

QAction* MainToolBar::addRegistry(const std::string& actionId, const nlohmann::json& params, const std::string& iconName, const QString& text) {
    const actions::ActionDef* def = win_.registry().find(actionId);
    if (!def) return nullptr;
    auto* a = new QAction(icon(iconName), text.isEmpty() ? qs(def->label) : text, this);
    a->setCheckable(def->kind == actions::ActionKind::Toggle);
    a->setObjectName(qs("mainToolbar:" + actionId + (params.is_null() ? std::string() : ":" + params.dump())));
    const nlohmann::json p = params;
    connect(a, &QAction::triggered, this, [this, actionId, p] {
        win_.runAction(actionId, p);
        refresh();
    });
    addAction(a);
    items_.push_back({a, actionId, p, iconName});
    return a;
}

void MainToolBar::buildCombo(QComboBox* box, const std::vector<double>& values, const QString& unit) {
    for (const double v : values) box->addItem(qs(tools::shortNumber(v)) + unit, v);
    box->setMinimumContentsLength(5);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
}

QAction* MainToolBar::find(const std::string& actionId, const nlohmann::json& params) const {
    for (const Item& it : items_)
        if (it.actionId == actionId && (params.is_null() || it.params == params)) return it.qaction;
    return nullptr;
}

void MainToolBar::refresh() {
    if (syncing_) return;
    syncing_ = true;
    UiEnv ui = win_.uiEnv();
    actions::ActionEnv env;
    env.app = &ui;
    for (const Item& it : items_) {
        const actions::ActionDef* def = win_.registry().find(it.actionId);
        if (!def) continue;
        it.qaction->setEnabled(win_.registry().isEnabled(*def, env, it.params));
        if (it.qaction->isCheckable()) it.qaction->setChecked(win_.registry().isChecked(*def, env, it.params));
        const std::string keys = win_.keymap().displayText(it.actionId, def->variants.empty() ? nlohmann::json(nullptr) : it.params);
        std::string label = def->label;
        if (!def->variants.empty())
            for (const actions::MenuVariant& v : def->variants)
                if (v.params == it.params) {
                    label.clear();
                    for (const char c : v.label)
                        if (c != '&') label += c;
                    if (it.actionId == "gizmo.mode") label += " gizmo";
                    else if (it.actionId == "tool.set") label += " tool";
                    else if (it.actionId == "view.layout") label += " (viewports)";
                    break;
                }
        QString tip = qs(label);
        if (!keys.empty()) tip += qs(" (" + keys + ")");
        if (!def->description.empty()) tip += QStringLiteral("\n") + qs(def->description);
        it.qaction->setToolTip(tip);
    }
    const bool mesh = win_.meshMode();
    const bool haveDoc = win_.document() != nullptr;
    object_->setChecked(!mesh);
    mesh_->setChecked(mesh);
    object_->setEnabled(haveDoc);
    mesh_->setEnabled(haveDoc);
    object_->setToolTip(QStringLiteral("Object mode: select and move whole brushes and entities (Tab toggles Object / Mesh)"));
    mesh_->setToolTip(QStringLiteral("Mesh mode: edit the vertices, edges and faces of the selected brush (Tab toggles Object / Mesh)"));
    for (QAction* a : elements_) a->setVisible(mesh);
    if (shapeAction_) shapeAction_->setText(titled(tools::shapeName(win_.shape())));

    const render::ViewportWindow* vp = win_.activeViewport();
    grid_->setEnabled(vp != nullptr);
    rotation_->setEnabled(vp != nullptr);
    if (vp) {
        const auto pick = [](QComboBox* box, double value) {
            int best = -1;
            for (int i = 0; i < box->count(); ++i)
                if (std::abs(box->itemData(i).toDouble() - value) < 1e-9) best = i;
            if (best >= 0) box->setCurrentIndex(best);
        };
        pick(grid_, vp->rig().settings().gridSize);
        pick(rotation_, vp->rig().settings().rotSnap);
    }
    const std::string keys = win_.keymap().displayText("view.command_palette");
    search_->setText(QStringLiteral("Search actions") + (keys.empty() ? QString() : qs("  " + keys)));
    search_->setToolTip(QStringLiteral("Find any command by name and run it"));
    syncing_ = false;
}

void MainToolBar::refreshIcons() {
    if (!object_ || !mesh_ || !search_) return;   // a style event while the bar is still being built
    for (const Item& it : items_) it.qaction->setIcon(icon(it.icon));
    object_->setIcon(icon("object"));
    mesh_->setIcon(icon("mesh"));
    search_->setIcon(icon("search"));
}

void MainToolBar::changeEvent(QEvent* e) {
    QToolBar::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::StyleChange) refreshIcons();
}

} // namespace native::ui
