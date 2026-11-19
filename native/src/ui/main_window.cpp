// ui/main_window -- see main_window.h.
#include "ui/main_window.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <algorithm>

#include "render/viewport_window.h"
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
    buildMenus();
}

MainWindow::~MainWindow() {
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
    for (const ViewportPanel& p : panels_) {
        p.window->setChangedCallback({});
        p.window->setActivatedCallback({});
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
    if (layoutGroup_)
        for (QAction* a : layoutGroup_->actions()) a->setChecked(a->data().toInt() == static_cast<int>(preset));
    scheduleSave();
}

void MainWindow::updateDockTitle(size_t index) {
    if (index >= panels_.size()) return;
    panels_[index].dock->setWindowTitle(titleFor(*panels_[index].window));
}

// ---- menus ------------------------------------------------------------------------------------------------------------------

void MainWindow::buildMenus() {
    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    if (!panels_.empty()) {
        QMenu* layout = view->addMenu(QStringLiteral("&Layout"));
        layoutGroup_ = new QActionGroup(this);
        const std::pair<const char*, LayoutPreset> items[] = {
            {"&1 viewport", LayoutPreset::One}, {"&2 viewports", LayoutPreset::Two}, {"&4 viewports", LayoutPreset::Four}};
        for (const auto& [text, preset] : items) {
            QAction* a = layout->addAction(QString::fromLatin1(text));
            a->setCheckable(true);
            a->setData(static_cast<int>(preset));
            a->setChecked(preset == preset_);
            layoutGroup_->addAction(a);
            QObject::connect(a, &QAction::triggered, this, [this, preset] { setLayoutPreset(preset); });
        }
        QMenu* camera = view->addMenu(QStringLiteral("&Camera (active viewport)"));
        cameraGroup_ = new QActionGroup(this);
        const std::pair<const char*, render::CameraMode> modes[] = {
            {"&Fly", render::CameraMode::Fly}, {"&Orbit", render::CameraMode::Orbit}, {"&Top", render::CameraMode::Top},
            {"F&ront", render::CameraMode::Front}, {"&Side", render::CameraMode::Side}};
        for (const auto& [text, mode] : modes) {
            QAction* a = camera->addAction(QString::fromLatin1(text));
            a->setCheckable(true);
            cameraGroup_->addAction(a);
            QObject::connect(a, &QAction::triggered, this, [this, mode] {
                if (auto* v = activeViewport()) v->setCameraMode(mode);
            });
        }
        QObject::connect(camera, &QMenu::aboutToShow, this, [this, camera] {
            const auto* v = activeViewport();
            const auto acts = camera->actions();
            const render::CameraMode order[] = {render::CameraMode::Fly, render::CameraMode::Orbit, render::CameraMode::Top,
                                                render::CameraMode::Front, render::CameraMode::Side};
            for (int i = 0; i < acts.size() && i < 5; ++i) acts[i]->setChecked(v && v->rig().mode() == order[i]);
        });
        view->addSeparator();
    }
    view->addAction(statsDock_->toggleViewAction());
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
