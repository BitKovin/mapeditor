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
#include "doc/selection.h"
#include "render/viewport_window.h"
#include "ui/input_adapter.h"
#include "ui/registry_menus.h"
#include "ui/stats_panel.h"

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
    }
    selection_ = std::make_unique<doc::Selection>();
    selection_->setListener([this] { emit selectionChanged(); });
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
    });
    completedTimer_.setInterval(25);
    QObject::connect(&completedTimer_, &QTimer::timeout, this, [this] { drainCompleted(); });
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
    if (vp) {
        const std::vector<std::string> v = viewportContexts(*vp);
        contexts.insert(contexts.end(), v.begin(), v.end());
    }
    router_.contexts().set(contexts);
}

bool MainWindow::routeKey(const QKeyEvent& e, bool press) {
    // A hold in progress keeps the viewport it started in; otherwise the viewport under the pointer gives the contexts.
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
    setContextsFor(vp);
    UiEnv ui{this, vp};
    actions::ActionEnv env{&ui};
    const bool before = router_.holding();
    const auto r = router_.handle(ev, env);
    if (ev.type == actions::InputEvent::Type::MousePress && r.handled && router_.holding() && !before) holdViewport_ = vp;
    if (!router_.holding()) holdViewport_ = nullptr;
    return r.handled;
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
