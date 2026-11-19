// ui/main_window -- see main_window.h.
#include "ui/main_window.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMetaObject>
#include <QMouseEvent>
#include <QSaveFile>
#include <QSettings>
#include <QString>
#include <QWheelEvent>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <algorithm>

#include "base/log.h"
#include "doc/commands.h"
#include "doc/prefabs.h"
#include "doc/selection.h"
#include "core/entdef.h"
#include "render/entity_overlay.h"
#include "render/picking.h"
#include "render/render_thread.h"
#include "render/selection_outline.h"
#include "render/viewport_window.h"
#include "scene/builder.h"
#include "scene/point_box.h"
#include "scene/size_labels.h"
#include "ui/glyph_atlas_qt.h"
#include "ui/input_adapter.h"
#include "ui/registry_menus.h"
#include "ui/box_drawing.h"
#include "ui/stats_panel.h"
#include "ui/transform_session.h"

namespace native::ui {

namespace {

const char* const kPreset = "layout/preset";
const char* const kViewports = "layout/viewports";
const char* const kDockState = "layout/dockState";
const char* const kGeometry = "window/geometry";
constexpr int kStateVersion = 1;
constexpr int kStatsWidth = 440;

int viewportCount(LayoutPreset p) { return static_cast<int>(p); }

// Camera mode of the n-th viewport of a fresh layout: perspective, top, front, side.
render::CameraMode defaultModeFor(int index) {
    static const render::CameraMode modes[4] = {render::CameraMode::Fly, render::CameraMode::Top, render::CameraMode::Front,
                                                render::CameraMode::Side};
    return modes[index % 4];
}

QString titleFor(const render::ViewportWindow& w) {
    switch (w.rig().mode()) {
        case render::CameraMode::Fly: return QStringLiteral("Perspective");
        case render::CameraMode::Orbit: return QStringLiteral("Perspective (orbit)");
        case render::CameraMode::Top: return QStringLiteral("Top");
        case render::CameraMode::Front: return QStringLiteral("Front");
        case render::CameraMode::Side: return QStringLiteral("Side");
    }
    return QStringLiteral("Viewport");
}

LayoutPreset presetFromInt(int v) { return v == 4 ? LayoutPreset::Four : (v == 2 ? LayoutPreset::Two : LayoutPreset::One); }

} // namespace

const char* MainWindow::windowTitle() { return "Map Editor (native)"; }

MainWindow::MainWindow(const Services& services, QWidget* parent) : QMainWindow(parent), services_(services) {
    setWindowTitle(QString::fromLatin1(windowTitle()));
    resize(1280, 800);

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, false);
    dockManager_ = new ads::CDockManager(this);   // becomes the central widget

    statsDock_ = new ads::CDockWidget(dockManager_, QStringLiteral("Stats"));
    statsDock_->setObjectName(QStringLiteral("statsDock"));
    stats_ = new StatsPanel(statsDock_);
    statsDock_->setWidget(stats_);

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(600);
    QObject::connect(&saveTimer_, &QTimer::timeout, this, [this] { saveLayout(); });

    const bool haveViewports = services_.renderThread && services_.vulkanInstance;
    QSettings* st = services_.settings;
    if (st && services_.forceViewports == 0) {
        const QByteArray geom = st->value(kGeometry).toByteArray();
        if (!geom.isEmpty()) restoreGeometry(geom);
    }

    if (haveViewports) {
        LayoutPreset preset = LayoutPreset::One;
        std::vector<SavedViewport> saved;
        if (services_.forceViewports != 0) {
            preset = presetFromInt(services_.forceViewports);
        } else if (st) {
            preset = presetFromInt(st->value(kPreset, 1).toInt());
            const nlohmann::json j = nlohmann::json::parse(st->value(kViewports).toString().toStdString(), nullptr, false);
            if (j.is_array())
                for (const auto& v : j)
                    if (v.is_object()) {
                        SavedViewport sv;
                        if (v.contains("settings") && v["settings"].is_string()) sv.settingsJson = v["settings"].get<std::string>();
                        if (v.contains("pose") && v["pose"].is_string()) sv.poseJson = v["pose"].get<std::string>();
                        saved.push_back(sv);
                    }
        }
        buildViewports(preset, saved);
        if (st && services_.forceViewports == 0) {
            const QByteArray state = st->value(kDockState).toByteArray();
            if (!state.isEmpty()) dockManager_->restoreState(state, kStateVersion);
        }
    } else {
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, statsDock_);   // widget-only tests
    }
    // Documents: the assets root (--assets, else the saved one), the recent list, the job results timer.
    assetsRoot_ = services_.assetsRoot;
    if (st) {
        if (assetsRoot_.empty()) assetsRoot_ = st->value("assets/root").toString().toStdString();
        for (const QString& r : st->value("files/recent").toStringList()) recent_.push_back(r.toStdString());
        {   // the hide-by-class rules are a view setting that stays between runs (the hidden keys do not: they belong to one map)
            const std::string text = st->value("view/hideRules").toString().toStdString();
            if (!text.empty()) hide_.rules = tools::restoreHideRules(nlohmann::json::parse(text, nullptr, false));
        }
    }
    prefabLib_ = std::make_shared<doc::PrefabLibrary>(assetsRoot_);
    prefabResolver_ = std::make_shared<doc::PrefabResolver>(*prefabLib_);
    prefabTimer_.setInterval(300);
    QObject::connect(&prefabTimer_, &QTimer::timeout, this, [this] { pollPrefabs(); });
    prefabTimer_.start();
    selection_ = std::make_unique<doc::Selection>();
    selection_->setListener([this] { emit selectionChanged(); });
    QObject::connect(this, &MainWindow::selectionChanged, this, [this] {
        if (!transform_ && !originSel_.empty()) originSel_.clear();   // the origin editing ends with any selection change
        refreshOutline();
    });
    history_ = std::make_unique<doc::History>();
    history_->setListener([this](const std::shared_ptr<const doc::DocumentVersion>& current, const doc::ChangeSet& changes, doc::ChangeCause cause) {
        document_ = current;
        // The selection follows the document before anyone is told about the new version: panels then see both consistent.
        if (!current || cause == doc::ChangeCause::Reset) selection_->reset();
        else selection_->follow(*current, cause == doc::ChangeCause::Undo || cause == doc::ChangeCause::Redo);
        updateTitle();
        if (menus()) menus()->refresh();
        emit documentChanged();
        emit documentEdited(changes);
        updatePrefabs();
        submitScene(changes);
        updateSky();
        if (!transform_) refreshGizmos();
    });
    completedTimer_.setInterval(25);
    QObject::connect(&completedTimer_, &QTimer::timeout, this, [this] { drainCompleted(); });
    QObject::connect(this, &MainWindow::prefabExpansionChanged, this, [this] {
        // the expansion of the instances arrived (or changed on disk): the objects of those instances are rebuilt
        if (!document_ || document_->instances.empty()) return;
        doc::ChangeSet cs;
        cs.fromVersion = cs.toVersion = document_->version;
        for (const auto& n : document_->instances) {
            doc::ChangedEntity ce;
            ce.instance = n->typed.id;
            ce.kinds = doc::Prefab;
            ce.before.add(n->typed.origin);
            ce.after.add(n->typed.origin);
            cs.entities.push_back(std::move(ce));
        }
        submitScene(cs);
    });
    setupScene();
    updateTitle();

    registerAppActions(registry_);
    loadKeymap();
    menus_ = new RegistryMenus(menuBar(), registry_, keymap_, [this] { return currentUiEnv(); }, this);
    menus_->build();
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
        QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) cancelInput();
        });
    }
}

MainWindow::~MainWindow() {
    builder_.reset();   // waits for its jobs; its listener points at the render thread
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
    saveTimer_.stop();
    // The dock manager (a child) deletes the docks, the containers and so the viewport windows, which release their swapchains.
}

render::ViewportWindow* MainWindow::activeViewport() const {
    if (panels_.empty()) return nullptr;
    return panels_[static_cast<size_t>(std::clamp(active_, 0, static_cast<int>(panels_.size()) - 1))].window;
}

void MainWindow::setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows) { stats_->setRows(rows); }

// ---- viewports ----------------------------------------------------------------------------------------------------------------

void MainWindow::buildViewports(LayoutPreset preset, const std::vector<SavedViewport>& saved) {
    building_ = true;
    preset_ = preset;
    const int n = viewportCount(preset);
    for (int i = 0; i < n; ++i) {
        render::ViewportSettings settings = render::defaultViewportSettings();
        settings.cameraMode = defaultModeFor(i);
        const bool have = static_cast<size_t>(i) < saved.size();
        if (have && !saved[static_cast<size_t>(i)].settingsJson.empty())
            settings = render::settingsFromJson(saved[static_cast<size_t>(i)].settingsJson);
        auto* window = new render::ViewportWindow(*services_.renderThread, *services_.vulkanInstance, settings);
        if (have && !saved[static_cast<size_t>(i)].poseJson.empty()) window->rig().poseFromJson(saved[static_cast<size_t>(i)].poseJson);
        QWidget* container = QWidget::createWindowContainer(window, nullptr);   // the container owns the window
        container->setMinimumSize(160, 120);
        container->setFocusPolicy(Qt::StrongFocus);
        auto* dock = new ads::CDockWidget(dockManager_, QString());
        dock->setObjectName(QStringLiteral("viewport%1").arg(i + 1));
        dock->setWidget(container, ads::CDockWidget::ForceNoScrollArea);
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        panels_.push_back({dock, container, window});
        const size_t index = panels_.size() - 1;
        window->setChangedCallback([this, index] {
            refreshSizeLabels(index);
            refreshGizmo(index);
            updateDockTitle(index);
            scheduleSave();
        });
        window->setActivatedCallback([this, index] { active_ = static_cast<int>(index); });
        window->setInputHook([this, index](QEvent* e, QPointF delta) { return routeViewportEvent(index, e, delta); });
        window->setCancelCallback([this] { cancelInput(); });
        updateDockTitle(index);
    }
    // 1 = one viewport; 2 = side by side; 4 = a 2 x 2 grid (perspective top-left, top, front, side).
    ads::CDockAreaWidget* a0 = dockManager_->addDockWidget(ads::CenterDockWidgetArea, panels_[0].dock);
    ads::CDockAreaWidget *a1 = nullptr, *a2 = nullptr, *a3 = nullptr;
    if (n >= 2) {
        a1 = dockManager_->addDockWidget(ads::RightDockWidgetArea, panels_[1].dock, a0);
        if (n == 4) {
            a2 = dockManager_->addDockWidget(ads::BottomDockWidgetArea, panels_[2].dock, a0);
            a3 = dockManager_->addDockWidget(ads::BottomDockWidgetArea, panels_[3].dock, a1);
        }
    }
    ads::CDockAreaWidget* statsArea = dockManager_->addDockWidget(ads::RightDockWidgetArea, statsDock_);
    // Equal viewport cells, the Stats dock at a fixed width on the right.
    if (a2) dockManager_->setSplitterSizes(a0, {1000, 1000});
    if (a3) dockManager_->setSplitterSizes(a1, {1000, 1000});
    const int columns = n == 1 ? 1 : 2;
    const int cell = std::max(200, (width() - kStatsWidth) / columns);
    QList<int> root;
    for (int c = 0; c < columns; ++c) root << cell;
    root << kStatsWidth;
    dockManager_->setSplitterSizes(statsArea, root);
    active_ = 0;
    building_ = false;
}

void MainWindow::destroyViewports() {
    cancelInput();
    for (const ViewportPanel& p : panels_) {
        p.window->setChangedCallback({});
        p.window->setActivatedCallback({});
        p.window->setInputHook({});
        p.window->setCancelCallback({});
        dockManager_->removeDockWidget(p.dock);
        delete p.dock;   // deletes the container and with it the viewport window (its swapchain is released first)
    }
    panels_.clear();
}

std::vector<MainWindow::SavedViewport> MainWindow::captureViewports() const {
    std::vector<SavedViewport> out;
    for (const ViewportPanel& p : panels_)
        out.push_back({render::settingsToJson(p.window->rig().settings()), p.window->rig().poseToJson()});
    return out;
}

void MainWindow::setLayoutPreset(LayoutPreset preset) {
    if (!services_.renderThread || !services_.vulkanInstance) return;
    if (preset == preset_ && !panels_.empty()) return;
    const std::vector<SavedViewport> saved = captureViewports();
    dockManager_->removeDockWidget(statsDock_);
    destroyViewports();
    buildViewports(preset, saved);
    if (menus_) menus_->refresh();
    scheduleSave();
}

void MainWindow::updateDockTitle(size_t index) {
    if (index >= panels_.size()) return;
    panels_[index].dock->setWindowTitle(titleFor(*panels_[index].window));
}

// ---- actions and input --------------------------------------------------------------------------------------------------------

UiEnv MainWindow::currentUiEnv() const {
    UiEnv u;
    u.window = const_cast<MainWindow*>(this);
    u.viewport = activeViewport();
    return u;
}

void MainWindow::loadKeymap() {
    keymapError_.clear();
    const auto readFile = [](const std::string& path, std::string& out) {
        QFile f(QString::fromStdString(path));
        if (path.empty() || !f.open(QIODevice::ReadOnly)) return false;
        out = f.readAll().toStdString();
        return true;
    };
    std::string text;
    if (!services_.keymapPresetPath.empty()) {
        if (!readFile(services_.keymapPresetPath, text)) {
            keymapError_ = "keymap preset not found: " + services_.keymapPresetPath;
        } else if (!keymap_.loadPreset(text, &keymapError_)) {
            keymapError_ = services_.keymapPresetPath + ": " + keymapError_;
        }
    }
    if (keymapError_.empty() && readFile(services_.userKeymapPath, text) && !keymap_.loadUser(text, &keymapError_))
        keymapError_ = services_.userKeymapPath + ": " + keymapError_;
    if (!keymapError_.empty()) base::logError("keymap: " + keymapError_);
}

bool MainWindow::saveUserKeymap() const {
    if (services_.userKeymapPath.empty()) return false;
    const QFileInfo info(QString::fromStdString(services_.userKeymapPath));
    QDir().mkpath(info.absolutePath());
    QSaveFile f(info.absoluteFilePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    const std::string text = keymap_.userToJson();
    f.write(text.data(), static_cast<qint64>(text.size()));
    return f.commit();
}

render::ViewportWindow* MainWindow::viewportUnderCursor() const {
    const QPoint global = QCursor::pos();
    for (const ViewportPanel& p : panels_)
        if (p.container->isVisible() && p.container->rect().contains(p.container->mapFromGlobal(global))) return p.window;
    return nullptr;
}

void MainWindow::setContextsFor(render::ViewportWindow* vp) {
    std::vector<std::string> contexts = {"window:main"};
    if (transform_ && transform_->source == TransformSession::Source::Modal) {
        contexts.push_back("modal:transform");   // a running modal operator owns the keys and the mouse buttons: no viewport context
    } else if (vp) {
        const std::vector<std::string> v = viewportContexts(*vp);
        contexts.insert(contexts.end(), v.begin(), v.end());
        contexts.push_back(tool_ == Tool::Box ? "tool:box" : "tool:select");
    }
    router_.contexts().set(contexts);
}

bool MainWindow::routeKey(const QKeyEvent& e, bool press) {
    // A hold in progress keeps the viewport it started in; otherwise the viewport under the pointer gives the contexts.
    if (box_ && (e.key() == Qt::Key_Shift)) updateBoxPreview();   // Shift squares the rectangle / makes the cube
    if (transformKey(e, press)) return true;
    render::ViewportWindow* vp = router_.holding() && holdViewport_ ? holdViewport_ : viewportUnderCursor();
    setContextsFor(vp);
    UiEnv ui{this, vp ? vp : activeViewport()};
    actions::ActionEnv env{&ui};
    const auto r = router_.handle(fromKeyEvent(e, press, isTextInput(QApplication::focusWidget())), env);
    if (!router_.holding()) holdViewport_ = nullptr;
    return r.handled;
}

bool MainWindow::routeViewportEvent(size_t index, QEvent* e, QPointF delta) {
    if (index >= panels_.size()) return false;
    render::ViewportWindow* vp = panels_[index].window;
    actions::InputEvent ev;
    switch (e->type()) {
        case QEvent::Wheel: ev = fromWheelEvent(*static_cast<QWheelEvent*>(e)); break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove: ev = fromMouseEvent(*static_cast<QMouseEvent*>(e), delta); break;
        default: return false;
    }
    if (boxMouse(index, ev)) return true;
    if (transformMouse(index, ev)) return true;
    setContextsFor(vp);
    UiEnv ui{this, vp};
    actions::ActionEnv env{&ui};
    const bool before = router_.holding();
    const auto r = router_.handle(ev, env);
    if (ev.type == actions::InputEvent::Type::MousePress && r.handled && router_.holding() && !before) holdViewport_ = vp;
    if (!router_.holding()) holdViewport_ = nullptr;
    return r.handled;
}

// ---- picking and selecting (T3.5) -----------------------------------------------------------------------------------------------

void MainWindow::onSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot) {
    if (!picker_) picker_ = std::make_unique<render::Picker>();
    picker_->setSnapshot(std::move(snapshot));
    refreshHidden();   // also refreshes the outline when it prunes the selection; otherwise:
    refreshOutline();
}

void MainWindow::refreshOutline() {
    if (!services_.renderThread) return;
    std::vector<render::LineVertex> lines;
    if (picker_ && picker_->snapshot()) lines = render::selectionOutline(*picker_->snapshot(), selection_->items());
    services_.renderThread->setSelectionLines(lines.empty() ? nullptr : std::make_shared<const std::vector<render::LineVertex>>(std::move(lines)));
    refreshOverlays();
}

// ---- entity overlays and size labels (T3.6) ----------------------------------------------------------------------------------------

void MainWindow::refreshOverlays() {
    if (!services_.renderThread || !picker_ || !picker_->snapshot()) return;
    if (glyphScale_ != devicePixelRatio()) {   // the overlay text is drawn 1:1 in device pixels: remake the atlas for this screen
        glyphScale_ = devicePixelRatio();
        services_.renderThread->setGlyphAtlas(makeGlyphAtlas(glyphScale_));
    }
    render::EntityOverlayOptions opt;
    opt.origins = opt.directions = opt.links = opt.lightGizmos = false;
    opt.hidden = hiddenSet_.get();
    for (const ViewportPanel& p : panels_) {   // a viewport that shows something asks for it in every viewport (one shared list)
        const render::ViewportSettings& s = p.window->rig().settings();
        opt.origins = opt.origins || s.showOrigins;
        opt.directions = opt.directions || s.showDirections;
        opt.links = opt.links || s.showLinks;
        opt.lightGizmos = opt.lightGizmos || s.showLightGizmos;
    }
    services_.renderThread->setEntityOverlay(
        std::make_shared<const render::EntityOverlay>(render::buildEntityOverlay(*picker_->snapshot(), selection_->items(), entityDefs_.get(), opt)));
    for (size_t i = 0; i < panels_.size(); ++i) refreshSizeLabels(i);
    refreshGizmos();
}

std::vector<render::TextItem> MainWindow::sizeLabelsFor(size_t index) {
    std::vector<render::TextItem> items;
    if (index >= panels_.size()) return items;
    render::ViewportWindow* vp = panels_[index].window;
    if (!vp) return items;
    const render::ViewportSettings& st = vp->rig().settings();
    const double dpr = vp->devicePixelRatio();
    const double W = vp->size().width() * dpr, H = vp->size().height() * dpr;
    if (st.showSizes && document_ && selection_ && !selection_->items().empty() && W >= 1 && H >= 1) {
        const render::ViewState view = vp->currentView();
        const render::Mat4 inv = render::mat4Invert(view.viewProj);
        const auto project = [&](const scene::V3& p) -> std::optional<scene::ScreenPoint> {
            const render::Mat4& m = view.viewProj;
            const double w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            if (w <= 1e-9) return std::nullopt;
            const double x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w;
            const double y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
            return scene::ScreenPoint{(x * 0.5 + 0.5) * W, (y * 0.5 + 0.5) * H};
        };
        const auto mpp = [&](const scene::V3& p) -> double {
            const render::Vec3 n = render::mat4Transform(view.viewProj, {p[0], p[1], p[2]});
            const render::Vec3 a = render::mat4Transform(inv, n), b = render::mat4Transform(inv, {n.x + 2.0 / W, n.y, n.z});
            return std::max(1e-6, render::length(b - a));
        };
        std::vector<scene::SizeLabel> labels;
        const std::vector<doc::SelRef>& sel = selection_->items();
        if (sel.size() == 1 && sel[0].isFace() && sel[0].instancePath.empty()) {
            const int bi = document_->findBrush(sel[0].entityId, sel[0].brushId);
            const int ei = document_->findEntity(sel[0].entityId);
            if (bi >= 0 && ei >= 0) {
                const core::Brush& b = document_->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
                for (const core::BrushFace& f : b.faces)
                    if (f.id == *sel[0].faceId) labels = scene::withoutOverlaps(scene::faceSizeLabels(b, f, mpp), project);
            }
        } else {
            scene::V3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
            const auto add = [&](const core::Brush& b) {
                core::Vec3 a, c;
                if (!b.bounds(a, c)) return;
                mn = {std::min(mn[0], a.x), std::min(mn[1], a.y), std::min(mn[2], a.z)};
                mx = {std::max(mx[0], c.x), std::max(mx[1], c.y), std::max(mx[2], c.z)};
            };
            for (const doc::SelRef& r : sel) {
                if (!r.instancePath.empty() || r.entityId.empty()) continue;
                const int ei = document_->findEntity(r.entityId);
                if (ei < 0) continue;
                const auto& node = *document_->entities[static_cast<size_t>(ei)];
                if (r.brushId.empty()) {
                    for (const auto& b : node.brushes) add(b->brush);
                } else {
                    const int bi = document_->findBrush(r.entityId, r.brushId);
                    if (bi >= 0) add(node.brushes[static_cast<size_t>(bi)]->brush);
                }
            }
            if (mn[0] <= mx[0]) {
                int skip = -1;
                if (!view.perspective) skip = view.gridPlane == 0 ? 1 : (view.gridPlane == 1 ? 2 : 0);
                labels = scene::boxSizeLabels(mn, mx, project, mpp, skip);
            }
        }
        for (const scene::SizeLabel& l : labels) items.push_back({l.text, l.at, 0xFFFFFFFFu});
    }
    return items;
}

void MainWindow::refreshSizeLabels(size_t index) {
    if (index >= panels_.size() || !services_.renderThread) return;
    render::ViewportWindow* vp = panels_[index].window;
    if (!vp || vp->viewportId() == 0) return;
    ViewportPanel& panel = panels_[index];
    std::vector<render::TextItem> items = sizeLabelsFor(index);
    if (items.empty() && !panel.hasSizeLabels) return;
    panel.hasSizeLabels = !items.empty();
    services_.renderThread->setSizeLabels(vp->viewportId(), items.empty() ? nullptr : std::make_shared<const std::vector<render::TextItem>>(std::move(items)));
}

namespace {
doc::SelRef objectRef(const doc::DocumentVersion& d, const doc::SelRef& r) { return doc::wholeEntityIfSole(d, r); }
} // namespace

void MainWindow::pickAt(render::ViewportWindow* vp, QPointF p, PickOp op) {
    if (!vp || !picker_ || !document_) return;
    const QSizeF size = vp->size();
    if (size.width() < 1 || size.height() < 1) return;
    const render::ViewState view = vp->currentView();
    const render::Ray ray = render::rayThrough(view.viewProj, p.x() / size.width() * 2.0 - 1.0, p.y() / size.height() * 2.0 - 1.0);
    const std::optional<render::PickHit> hit = picker_->pick(ray, pickOptions());
    switch (op) {
        case PickOp::Replace:
            if (hit) selection_->select(objectRef(*document_, hit->toSelRef(false)));
            else selection_->clear();
            break;
        case PickOp::Face:
            if (hit) selection_->select(hit->faceId && hit->instance.empty() ? hit->toSelRef(true) : objectRef(*document_, hit->toSelRef(false)));
            else selection_->clear();
            break;
        case PickOp::Toggle:
            if (hit) selection_->toggle(*document_, objectRef(*document_, hit->toSelRef(false)));
            break;
    }
}

void MainWindow::boxSelectIn(render::ViewportWindow* vp, QPointF a, QPointF b, PickOp op) {
    if (!vp || !picker_ || !document_) return;
    const QSizeF size = vp->size();
    if (size.width() < 1 || size.height() < 1) return;
    const render::ViewState view = vp->currentView();
    const auto nx = [&](double x) { return x / size.width() * 2.0 - 1.0; };
    const auto ny = [&](double y) { return y / size.height() * 2.0 - 1.0; };
    std::vector<doc::SelRef> list = picker_->boxSelect(view.viewProj, nx(a.x()), ny(a.y()), nx(b.x()), ny(b.y()), pickOptions());
    if (op != PickOp::Replace) {
        // Ctrl / Shift add to what is selected
        std::vector<doc::SelRef> all = selection_->items();
        for (const doc::SelRef& r : list)
            if (std::find(all.begin(), all.end(), r) == all.end()) all.push_back(r);
        list = std::move(all);
    }
    selection_->selectMany(*document_, list);
}

void MainWindow::pickInput(render::ViewportWindow* vp, const actions::InputEvent& e, PickOp op) {
    using Type = actions::InputEvent::Type;
    const QPointF p(e.x, e.y);
    if (e.type == Type::MousePress) {
        pickDrag_ = {vp, p, p, false};
        return;
    }
    if (pickDrag_.viewport != vp) return;
    if (e.type == Type::MouseMove) {
        pickDrag_.current = p;
        if (!pickDrag_.dragging && (p - pickDrag_.start).manhattanLength() >= 4) pickDrag_.dragging = true;
        if (pickDrag_.dragging) vp->setRubberBand(true, pickDrag_.start, p);
        return;
    }
    if (e.type == Type::MouseRelease) {
        const PickDrag drag = pickDrag_;
        pickDrag_ = {};
        if (drag.dragging) {
            vp->setRubberBand(false);
            boxSelectIn(vp, drag.start, p, op);
        } else {
            pickAt(vp, p, op);
        }
    }
}

void MainWindow::cancelInput() {
    if (!router_.holding() && !router_.sequencePending()) return;
    UiEnv ui{this, holdViewport_ ? holdViewport_ : activeViewport()};
    actions::ActionEnv env{&ui};
    router_.cancelAll(env, nowMs());
    holdViewport_ = nullptr;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* e) {
    if ((e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) && watched->isWindowType() && !QApplication::activePopupWidget() &&
        !QApplication::activeModalWidget()) {
        // A window gets the event first and then hands it to its focus widget (which the filter sees again): only the window pass counts.
        if (routeKey(*static_cast<QKeyEvent*>(e), e->type() == QEvent::KeyPress)) return true;
    }
    return QMainWindow::eventFilter(watched, e);
}

// ---- persistence ------------------------------------------------------------------------------------------------------------

void MainWindow::scheduleSave() {
    if (building_ || !services_.settings || services_.forceViewports != 0) return;
    saveTimer_.start();
}

void MainWindow::saveLayout() {
    QSettings* st = services_.settings;
    if (!st || services_.forceViewports != 0) return;
    st->setValue(kGeometry, saveGeometry());
    st->setValue(kPreset, static_cast<int>(preset_));
    nlohmann::json arr = nlohmann::json::array();
    for (const SavedViewport& v : captureViewports()) arr.push_back({{"settings", v.settingsJson}, {"pose", v.poseJson}});
    st->setValue(kViewports, QString::fromStdString(arr.dump()));
    st->setValue(kDockState, dockManager_->saveState(kStateVersion));
    st->sync();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveTimer_.stop();
    saveLayout();
    QMainWindow::closeEvent(e);
}

} // namespace native::ui
