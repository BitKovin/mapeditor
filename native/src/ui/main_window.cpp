// ui/main_window -- see main_window.h.
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "tools/parametric.h"

#include <DockAreaTitleBar.h>
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
#include <QVBoxLayout>
#include <QString>
#include <QWheelEvent>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

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
#include "light/light_link.h"
#include "light/lighting_service.h"
#include "scene/builder.h"
#include "scene/model_entity.h"
#include "scene/model_store.h"
#include "render/model_scene.h"
#include "scene/point_box.h"
#include "scene/size_labels.h"
#include "ui/glyph_atlas_qt.h"
#include "ui/input_adapter.h"
#include "ui/place_panel.h"
#include "ui/main_toolbar.h"
#include "ui/viewport_header.h"
#include "ui/registry_menus.h"
#include "ui/box_drawing.h"
#include "ui/stats_panel.h"
#include "ui/status_bar.h"
#include "ui/transform_session.h"

namespace native::ui {

namespace {

const char* const kPreset = "layout/preset";
const char* const kViewports = "layout/viewports";
const char* const kDockState = "layout/dockState";
const char* const kGeometry = "window/geometry";
constexpr int kStateVersion = 2;   // 2: the Place, Outliner and Details docks
constexpr int kStatsWidth = 400;
constexpr int kPlaceWidth = 250;

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
    if (services_.device)   // the handler runs on the thread that noticed the loss (render, compute): it only posts to the UI thread
        services_.device->setLostHandler([this](const std::string& why) {
            QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(why)] { onDeviceLost(text); }, Qt::QueuedConnection);
        });

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, false);
    // The look of the docking chrome comes from ui/theme (dockStyleSheet, part of the application style sheet): the system's own style sheet is off, and an area keeps no
    // close / float / tab-list buttons (a panel is closed from its tab's menu or View, floated by dragging its tab): 14_UI_REDESIGN.md section 9.2.
    ads::CDockManager::setConfigFlag(ads::CDockManager::DisableStylesheet, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::ActiveTabHasCloseButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHideDisabledButtons, true);
    dockManager_ = new ads::CDockManager(this);   // becomes the central widget

    statsDock_ = new ads::CDockWidget(dockManager_, QStringLiteral("Stats"));
    statsDock_->setObjectName(QStringLiteral("statsDock"));
    stats_ = new StatsPanel(statsDock_);
    statsDock_->setWidget(stats_);
    createToolDocks();

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(600);
    QObject::connect(&saveTimer_, &QTimer::timeout, this, [this] { saveLayout(); });

    const bool haveViewports = services_.renderThread && services_.vulkanInstance;
    QSettings* st = services_.settings;
    if (st) prefs_ = tools::prefsFromJson(st->value("prefs/editor").toString().toStdString());
    if (st) uvLock_ = st->value("edit/uvLock", true).toBool();
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
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, placeDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, outlinerDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, detailsDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, surfaceDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, texturesDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, groupsDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, levelDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, historyDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, messagesDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, adjustDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, lightingDock_);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, toolOptionsDock_);
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
    if (historyPanel_) historyPanel_->refreshNow();
    history_->setReconciler([](const std::shared_ptr<const doc::DocumentVersion>& prev, const std::shared_ptr<const doc::DocumentVersion>& next) {
        return tools::parametric::reconcileShapes(prev, next, [] { return tools::newId("s"); });
    });
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
    statusFields_ = new StatusFields(*this);
    setupScene();
    updateTitle();

    registerAppActions(registry_);
    initKeymapFiles();
    loadKeymap();
    menus_ = new RegistryMenus(menuBar(), registry_, keymap_, [this] { return currentUiEnv(); }, this);
    menus_->build();
    toolbar_ = new MainToolBar(*this);   // under the menu bar: the frequent actions as grouped icon buttons (14_UI_REDESIGN.md 6.2)
    addToolBar(Qt::TopToolBarArea, toolbar_);
    statusFields_->refreshNow();   // the hints need the registry and the keymap
    if (place_) place_->refreshTooltips();
    if (services_.watchInput) {
        inputTimer_.setInterval(16);
        QObject::connect(&inputTimer_, &QTimer::timeout, this, [this] {
            bool pointerLocked = false;
            bool flying = false;
            for (const ViewportPanel& p : panels_) {
                pointerLocked = pointerLocked || p.window->pointerGrabbed();
                flying = flying || p.window->rig().flying();
            }
            if (router_.holding() || pointerLocked || flying) reconcileInput();
        });
        inputTimer_.start();
    }
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
        QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) cancelInput();
            else if (services_.renderThread) services_.renderThread->refreshTextures();   // texture files edited while the editor was in the background
        });
    }
}

MainWindow::~MainWindow() {
    if (services_.device) services_.device->setLostHandler(nullptr);   // waits for a handler call in progress
    rtLights_.reset();   // its callback feeds the render thread
    entityLights_.reset();
    scene::setKeyDefaultProvider({});
    modelStore_.reset();   // uninstalls the bounds provider; a loading job only finds an empty listener
    lighting_.reset();   // the bake thread reads the link and the device
    if (services_.renderThread) services_.renderThread->setLightLink(nullptr);
    lightLink_.reset();
    builder_.reset();   // waits for its jobs; its listener points at the render thread
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
    saveTimer_.stop();
    // The dock manager (a child) deletes the docks, the containers and so the viewport windows, which release their swapchains.
}

render::ViewportWindow* MainWindow::activeViewport() const {
    if (panels_.empty()) return nullptr;
    return panels_[static_cast<size_t>(std::clamp(active_, 0, static_cast<int>(panels_.size()) - 1))].window;
}

bool MainWindow::lightingAvailable() const { return lighting_ && lighting_->valid(); }
bool MainWindow::litModesAvailable() const { return lightingAvailable() || (services_.rtLit && services_.renderThread && services_.device && services_.device->caps().lit()); }

bool MainWindow::upscalerAvailable() const {
    if (services_.renderThread) return services_.renderThread->upscalerKind() != gpu::UpscalerKind::None;
    return services_.device && services_.device->caps().upscaler();
}

bool MainWindow::lightingSettled() const {
    if (!lightingAvailable()) return true;
    const auto st = lighting_->status();
    return st.epoch > 0 && (st.converged || !st.error.empty());
}

std::string MainWindow::lightingText() const {
    if (!lighting_) return services_.device && !services_.device->caps().lit() ? "unavailable (no ray query device)" : "off";
    if (!lighting_->valid()) return "unavailable";
    const auto st = lighting_->status();
    char buf[160];
    if (!st.error.empty()) return "error: " + st.error;
    if (st.epoch == 0 && !st.building) return "waiting for a lit view";
    if (st.building) return "building the layout and samples";
    if (st.converged) {
        std::snprintf(buf, sizeof buf, "converged: %u chunks, %.1f s to build, %.1f s to refine", st.chunks, st.buildSeconds, st.refineSeconds);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "baking: %u / %u chunks", st.convergedChunks, st.chunks);
    return buf;
}

void MainWindow::refreshStats() {
    auto rows = deviceRows_;
    if (services_.device && services_.device->lost()) rows.emplace_back("GPU", "DEVICE LOST: " + services_.device->lostReason());
    rows.emplace_back("Lighting", lightingText());
    if (services_.device && services_.device->streamline()) {
        const gpu::StreamlineStatus sl = services_.device->streamline()->status();
        char buf[64];
        if (sl.reflexActive && sl.latencyMs > 0) {
            std::snprintf(buf, sizeof buf, "%.1f ms (sim start to GPU end)", sl.latencyMs);
            rows.emplace_back("Reflex latency", buf);
        } else if (!sl.note.empty()) {
            rows.emplace_back("Reflex latency", sl.note);
        }
    }
    if (services_.renderThread) {
        const auto info = services_.renderThread->sceneInfo();
        rows.emplace_back("Lit objects", std::to_string(info.scene.litObjects) + " of " + std::to_string(info.scene.objects));
        int n = 0;
        for (const ViewportPanel& panel : panels_) {   // one row per viewport that asks for DLSS
            ++n;
            if (!panel.window || panel.window->rig().settings().dlss == render::DlssMode::Off) continue;
            const auto d = services_.renderThread->dlssInfo(panel.window->viewportId());
            std::string text;
            const std::string name = d.backend != gpu::UpscalerKind::None ? gpu::upscalerName(d.backend) : gpu::upscalerName(services_.renderThread->upscalerKind());
            if (d.active)
                text = (d.version.empty() ? name : d.version) + " " + render::dlssModeName(static_cast<render::DlssMode>(d.mode)) + " " + std::to_string(d.renderW) + "x" +
                       std::to_string(d.renderH) + " -> " + std::to_string(d.outW) + "x" + std::to_string(d.outH);
            else if (d.failed) text = "off: " + name + " refused (see the log)";
            else if (!upscalerAvailable()) text = "off: no upscaler on this device (DLSS needs an RTX GPU, FSR needs the FidelityFX DLLs)";
            else text = "waiting for a perspective view";
            rows.emplace_back("Upscaling (viewport " + std::to_string(n) + ")", text);
        }
    }
    std::string key;
    for (const auto& r : rows) key += r.first + "" + r.second + "";
    if (key == statsShown_) return;
    statsShown_ = key;
    stats_->setRows(rows);
}

void MainWindow::setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows) {
    deviceRows_ = rows;
    statsShown_.clear();
    refreshStats();
    if (!statsTimer_.isActive()) {
        statsTimer_.setInterval(500);
        QObject::connect(&statsTimer_, &QTimer::timeout, this, [this] { refreshStats(); });
        statsTimer_.start();
    }
}

// ---- viewports ----------------------------------------------------------------------------------------------------------------

void MainWindow::buildViewports(LayoutPreset preset, const std::vector<SavedViewport>& saved) {
    building_ = true;
    preset_ = preset;
    const int n = viewportCount(preset);
    for (int i = 0; i < n; ++i) {
        render::ViewportSettings settings = tools::applyPrefs(prefs_, render::defaultViewportSettings());
        settings.cameraMode = defaultModeFor(i);
        const bool have = static_cast<size_t>(i) < saved.size();
        if (have && !saved[static_cast<size_t>(i)].settingsJson.empty())
            settings = render::settingsFromJson(saved[static_cast<size_t>(i)].settingsJson);
        auto* window = new render::ViewportWindow(*services_.renderThread, *services_.vulkanInstance, settings);
        if (have && !saved[static_cast<size_t>(i)].poseJson.empty()) window->rig().poseFromJson(saved[static_cast<size_t>(i)].poseJson);
        QWidget* container = QWidget::createWindowContainer(window, nullptr);   // the container owns the window
        container->setMinimumSize(160, 120);
        container->setFocusPolicy(Qt::StrongFocus);
        auto* holder = new QWidget;   // the header strip above the viewport (the viewport is a native window: nothing can be drawn over it)
        auto* column = new QVBoxLayout(holder);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto* dock = new ads::CDockWidget(dockManager_, QString());
        dock->setObjectName(QStringLiteral("viewport%1").arg(i + 1));
        dock->setWidget(holder, ads::CDockWidget::ForceNoScrollArea);
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        panels_.push_back({dock, container, window});
        const size_t index = panels_.size() - 1;
        panels_[index].holder = holder;
        panels_[index].header = new ViewportHeader(*this, index, holder);
        column->addWidget(panels_[index].header);
        column->addWidget(container, 1);
        window->setChangedCallback([this, index] {
            refreshSizeLabels(index);
            refreshGizmo(index);
            updateDockTitle(index);
            scheduleSave();
            emit viewportsChanged();
        });
        window->setActivatedCallback([this, index] {
            active_ = static_cast<int>(index);
            emit viewportsChanged();
        });
        window->setInputHook([this, index](QEvent* e, QPointF delta) { return routeViewportEvent(index, e, delta); });
        window->setCancelCallback([this] { cancelInput(); });
        window->setVerifyButtons(services_.verifyButtons);
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
    ads::CDockAreaWidget* placeArea = dockManager_->addDockWidget(ads::LeftDockWidgetArea, placeDock_);
    dockManager_->addDockWidget(ads::CenterDockWidgetArea, texturesDock_, placeArea);
    dockManager_->addDockWidget(ads::CenterDockWidgetArea, groupsDock_, placeArea);
    placeDock_->setAsCurrentTab();
    ads::CDockAreaWidget* statsArea = dockManager_->addDockWidget(ads::RightDockWidgetArea, statsDock_);
    // Equal viewport cells; the Place dock on the left and the Outliner / Details / Stats column on the right at fixed widths.
    if (a2) dockManager_->setSplitterSizes(a0, {1000, 1000});
    if (a3) dockManager_->setSplitterSizes(a1, {1000, 1000});
    const int columns = n == 1 ? 1 : 2;
    const int cell = std::max(200, (width() - kStatsWidth - kPlaceWidth) / columns);
    QList<int> root;
    root << kPlaceWidth;
    for (int c = 0; c < columns; ++c) root << cell;
    root << kStatsWidth;
    dockManager_->setSplitterSizes(statsArea, root);
    layoutToolDocks(statsArea);
    active_ = 0;
    maximised_ = -1;
    hideViewportTitleBars();
    building_ = false;
}

void MainWindow::destroyViewports() {
    cancelInput();
    viewportTitleBars_.clear();
    maximised_ = -1;
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
    if (maximised_ >= 0) toggleMaximizeViewport(static_cast<size_t>(maximised_));   // all docks back before the layout is rebuilt
    const std::vector<SavedViewport> saved = captureViewports();
    dockManager_->removeDockWidget(statsDock_);
    dockManager_->removeDockWidget(placeDock_);
    dockManager_->removeDockWidget(outlinerDock_);
    dockManager_->removeDockWidget(detailsDock_);
    dockManager_->removeDockWidget(surfaceDock_);
    dockManager_->removeDockWidget(texturesDock_);
    dockManager_->removeDockWidget(groupsDock_);
    dockManager_->removeDockWidget(levelDock_);
    dockManager_->removeDockWidget(historyDock_);
    dockManager_->removeDockWidget(messagesDock_);
    dockManager_->removeDockWidget(adjustDock_);
    dockManager_->removeDockWidget(lightingDock_);
    dockManager_->removeDockWidget(toolOptionsDock_);
    destroyViewports();
    buildViewports(preset, saved);
    if (menus_) menus_->refresh();
    scheduleSave();
}

void MainWindow::setActiveViewport(size_t index) {
    if (index >= panels_.size() || static_cast<int>(index) == active_) return;
    active_ = static_cast<int>(index);
    emit viewportsChanged();
}

void MainWindow::toggleMaximizeViewport(size_t index) {
    if (index >= panels_.size()) return;
    const bool restore = maximised_ == static_cast<int>(index);
    if (!restore && panels_.size() < 2) return;
    maximised_ = restore ? -1 : static_cast<int>(index);
    for (size_t i = 0; i < panels_.size(); ++i)
        if (i != index) panels_[i].dock->toggleView(restore);
    if (!restore) active_ = static_cast<int>(index);
    hideViewportTitleBars();
    emit viewportsChanged();
}

void MainWindow::floatViewport(size_t index) {
    if (index >= panels_.size()) return;
    panels_[index].dock->setFloating();
}

// The dock areas of the viewports show no title bar: the viewport header is their title. ADS shows a bar again when it rearranges the areas, so a Show event hides it once more.
void MainWindow::hideViewportTitleBars() {
    for (const ViewportPanel& p : panels_) {
        ads::CDockAreaWidget* area = p.dock->dockAreaWidget();
        if (!area || !area->titleBar()) continue;
        QWidget* bar = area->titleBar();
        bar->setVisible(false);
        const bool known = std::any_of(viewportTitleBars_.begin(), viewportTitleBars_.end(), [bar](const QPointer<QWidget>& w) { return w == bar; });
        if (!known) {
            bar->installEventFilter(this);
            viewportTitleBars_.push_back(bar);
        }
    }
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
    const std::string userPath = activeKeymapPath();
    if (keymapError_.empty() && readFile(userPath, text) && !keymap_.loadUser(text, &keymapError_))
        keymapError_ = userPath + ": " + keymapError_;
    if (!keymapError_.empty()) base::logError("keymap: " + keymapError_);
}

bool MainWindow::saveUserKeymap() const {
    const std::string userPath = activeKeymapPath();
    if (userPath.empty()) return false;
    const QFileInfo info(QString::fromStdString(userPath));
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
        const std::string mode = modeContext();
        contexts.push_back(mode == "tool:box" || mode == "tool:decal" ? mode : "tool:select");
        if (meshMode()) {   // the mesh bindings are more specific than the select tool's
            contexts.push_back("mode:mesh");
            contexts.push_back(meshElement() == tools::mesh::SelMode::Vertex ? "mode:mesh.vertex" : meshElement() == tools::mesh::SelMode::Edge ? "mode:mesh.edge" : "mode:mesh.face");
            if (knife_) contexts.push_back("mesh:knife");   // Enter cuts, Esc cancels
        }
        if (measure_) contexts.push_back("tool:measure");   // Esc ends it
        if (clip_) contexts.push_back("brush:clipper");   // Enter / Shift+Enter / Ctrl+Enter cut, Esc cancels
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
    const actions::InputEvent ev = fromKeyEvent(e, press, isTextInput(QApplication::focusWidget()));
    const auto r = router_.handle(ev, env);
    if (inputLogging())
        base::logInfo(std::string("input: key ") + (press ? "press " : "release ") + ev.key + " scan=" + ev.scan + (ev.autoRepeat ? " auto-repeat" : "") + " holding=" + std::to_string(router_.heldTriggers().size()) +
                      " handled=" + (r.handled ? "yes" : "no") + " viewport=" + (vp ? "under cursor" : "none"));
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
    if (ev.type == actions::InputEvent::Type::MousePress) {
        // a click in the viewport takes the keyboard from a text field (a search box, a property) so the camera keys reach the router again
        if (QWidget* f = QApplication::focusWidget(); f && isTextInput(f)) f->clearFocus();
    }
    if (!services_.verifyButtons) ev.buttonsKnown = false;
    if (boxMouse(index, ev)) return true;
    if (knifeMouse(index, ev)) return true;
    if (clipperMouse(index, ev)) return true;
    if (measureMouse(index, ev)) return true;
    if (transformMouse(index, ev)) return true;
    setContextsFor(vp);
    UiEnv ui{this, vp};
    actions::ActionEnv env{&ui};
    const bool before = router_.holding();
    const auto r = router_.handle(ev, env);
    if (inputLogging() && ev.type != actions::InputEvent::Type::MouseMove)   // wheel and buttons: what the router did with them and what stays held
        base::logInfo(std::string("input: ") + (ev.type == actions::InputEvent::Type::Wheel ? "wheel " + std::to_string(ev.wheel) : ev.type == actions::InputEvent::Type::MousePress ? "mouse press " + std::to_string(ev.button) : "mouse release " + std::to_string(ev.button)) +
                      " holding=" + std::to_string(router_.heldTriggers().size()) + " handled=" + (r.handled ? "yes" : "no") + " action=" + r.action);
    if (ev.type == actions::InputEvent::Type::MousePress && r.handled && router_.holding() && !before) holdViewport_ = vp;
    if (!router_.holding()) holdViewport_ = nullptr;
    return r.handled;
}

// ---- picking and selecting (T3.5) -----------------------------------------------------------------------------------------------

void MainWindow::feedEntityLights(const std::shared_ptr<const scene::SceneSnapshot>& snapshot) {
    // Only the lit views show it, and only where something makes light: the ray traced view or the lit bake
    if (!(services_.rtLit || lighting_) || !services_.renderThread || !profile_ || !snapshot || !snapshot->complete || !snapshot->doc) return;
    if (!entityLights_) {
        render::RenderThread* rt = services_.renderThread;
        entityLights_ = std::make_unique<render::EntityLightProducer>([rt](std::shared_ptr<const render::EntityLightTable> t) { rt->setEntityLights(std::move(t)); });
    }
    entityLights_->request({snapshot, profile_, assetsRoot_, lightFilter_, modelScene_});
}

void MainWindow::feedLighting(const std::shared_ptr<const scene::SceneSnapshot>& snapshot) {
    feedEntityLights(snapshot);
    if (services_.rtLit && services_.renderThread && profile_ && snapshot && snapshot->complete && snapshot->doc) {   // the level lights, collected off the UI thread
        if (!rtLights_) {
            render::RenderThread* rt = services_.renderThread;
            rtLights_ = std::make_unique<render::RtLightsProducer>([rt](std::shared_ptr<const render::RtLightData> d) { rt->setRtLights(std::move(d)); });
        }
        rtLights_->request({snapshot, profile_, assetsRoot_, lightFilter_});
    }
    if (lighting_ && profile_ && snapshot && snapshot->complete) {   // the lit modes' bake follows complete snapshots
        light::LightingInputs in;
        in.snapshot = snapshot;
        in.profile = profile_;
        in.assetsRoot = assetsRoot_;
        in.filter = lightFilter_;
        lighting_->setInputs(std::move(in));
    }
}

void MainWindow::onSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot) {
    if (!picker_) picker_ = std::make_unique<render::Picker>();
    if (services_.rtLit && services_.renderThread && snapshot && snapshot->doc) services_.renderThread->setRtLighting(render::rtLightingOf(*snapshot->doc));   // the level's ambient / occlusion / exposure
    if (services_.rtLit && services_.renderThread && profile_ && snapshot && snapshot->complete && snapshot->doc) {
        auto rules = render::makeRtRules(*snapshot, core::LightingClasses::fromProfile(*profile_));
        if (!rtRules_ || *rtRules_ != *rules) {
            rtRules_ = rules;
            services_.renderThread->setObjectRules(std::move(rules));
        }
    }
    feedLighting(snapshot);
    picker_->setSnapshot(std::move(snapshot));
    refreshModels();
    refreshHidden();   // also refreshes the outline when it prunes the selection; otherwise:
    refreshOutline();
}

// ---- models of entities (T11.9c) ---------------------------------------------------------------------------------------------------

void MainWindow::refreshModels() {
    if (!services_.renderThread || !modelStore_ || !picker_ || !picker_->snapshot()) return;
    modelScene_ = render::buildModelScene(*picker_->snapshot(), *modelStore_, hiddenSet_.get());
    services_.renderThread->setModels(modelScene_);
    feedEntityLights(picker_->snapshot());
}

void MainWindow::modelStoreChanged() {
    if (picker_ && picker_->snapshot()) picker_->setSnapshot(picker_->snapshot());   // the boxes of the entities that show the model changed
    refreshModels();
    refreshOutline();
}

void MainWindow::refreshOutline() {
    if (!services_.renderThread) return;
    std::vector<render::LineVertex> lines;
    if (meshMode() && meshTarget()) lines = meshEdgeLines();   // mesh mode shows the edges of the edited brush instead of the plain outline
    else if (picker_ && picker_->snapshot()) lines = render::selectionOutline(*picker_->snapshot(), selection_->items());
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

void MainWindow::selectHit(const render::PickHit& hit) {
    const doc::SelRef ref = objectRef(*document_, hit.toSelRef(false));
    // a click on any part of a placed shape selects the whole shape (REQUIREMENTS 22 P1)
    const std::vector<std::string> parts = ref.instancePath.empty() && !ref.brushId.empty() ? tools::parametric::shapeMembers(*document_, ref.entityId, ref.brushId) : std::vector<std::string>{};
    if (parts.size() > 1) {
        std::vector<doc::SelRef> refs;
        for (const std::string& b : parts) refs.push_back(doc::SelRef::brush(ref.entityId, b));
        selection_->selectMany(*document_, refs);
    } else {
        selection_->select(ref);
    }
}

void MainWindow::pickAt(render::ViewportWindow* vp, QPointF p, PickOp op) {
    if (!vp || !picker_ || !document_) return;
    const QSizeF size = vp->size();
    if (size.width() < 1 || size.height() < 1) return;
    const render::ViewState view = vp->currentView();
    const render::Ray ray = render::rayThrough(view.viewProj, p.x() / size.width() * 2.0 - 1.0, p.y() / size.height() * 2.0 - 1.0);
    const std::optional<render::PickHit> hit = picker_->pick(ray, pickOptions());
    switch (op) {
        case PickOp::Replace:
            if (hit) selectHit(*hit);
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
    if (router_.holding() || router_.sequencePending()) {
        UiEnv ui{this, holdViewport_ ? holdViewport_ : activeViewport()};
        actions::ActionEnv env{&ui};
        router_.cancelAll(env, nowMs());
        holdViewport_ = nullptr;
    }
    // whatever the gestures left in the viewports (pointer lock, hidden cursor, fly keys) goes too, held or not
    for (const ViewportPanel& p : panels_) p.window->resetInput();
}

void MainWindow::setVerifyButtons(bool on) {
    services_.verifyButtons = on;
    for (const ViewportPanel& p : panels_) p.window->setVerifyButtons(on);
}

void MainWindow::reconcileInput() { reconcileInput(hardwareTriggerDown); }

void MainWindow::reconcileInput(const InputProbe& probe) {
    if (router_.holding()) {
        UiEnv ui{this, holdViewport_ ? holdViewport_ : activeViewport()};
        actions::ActionEnv env{&ui};
        for (const std::string& trigger : router_.heldTriggers()) {
            bool known = false;
            const bool down = probe(trigger, known);
            if (known && !down) {
                if (inputLogging()) base::logInfo("input: " + trigger + " is up on the keyboard but was held: released by the probe");
                router_.release(trigger, env, nowMs());
            }
        }
        if (!router_.holding()) holdViewport_ = nullptr;
    }
    for (const ViewportPanel& p : panels_) {   // a camera that flies with nothing held (a release went to another viewport, or never came) stops
        if (!router_.holding() && p.window->rig().flying()) {
            if (inputLogging()) base::logInfo("input: a viewport flies with no key held: stopped");
            p.window->resetInput();
        }
    }
    for (const ViewportPanel& p : panels_) {   // a locked pointer needs its right or middle button
        if (!p.window->pointerGrabbed()) continue;
        bool knownRight = false, knownMiddle = false;
        const bool right = probe(actions::Router::mouseTrigger(2), knownRight);
        const bool middle = probe(actions::Router::mouseTrigger(3), knownMiddle);
        if (knownRight && knownMiddle && !right && !middle) p.window->resetInput();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* e) {
    if (e->type() == QEvent::Show && !viewportTitleBars_.empty()) {
        for (const QPointer<QWidget>& bar : viewportTitleBars_)
            if (bar == watched) {
                QWidget* w = bar;
                QTimer::singleShot(0, w, [w] { w->hide(); });
                break;
            }
    }
    if ((e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) && watched->isWindowType()) {
        const bool press = e->type() == QEvent::KeyPress;
        if (!QApplication::activePopupWidget() && !QApplication::activeModalWidget()) {
            // A window gets the event first and then hands it to its focus widget (which the filter sees again): only the window pass counts.
            if (routeKey(*static_cast<QKeyEvent*>(e), press)) return true;
        } else if (!press) {
            routeKey(*static_cast<QKeyEvent*>(e), false);   // a menu or dialog is up: a release must still end a hold that began before it (never swallowed)
        } else if (router_.holding()) {
            cancelInput();                                    // a key for the popup while something is held: the viewport will not see the releases
        }
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
    if (maximised_ < 0) st->setValue(kDockState, dockManager_->saveState(kStateVersion));   // a maximised viewport hides the others: keep the last full state
    st->sync();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveTimer_.stop();
    if (maximised_ >= 0) toggleMaximizeViewport(static_cast<size_t>(maximised_));   // the saved dock state has every viewport
    saveLayout();
    QMainWindow::closeEvent(e);
}

} // namespace native::ui
