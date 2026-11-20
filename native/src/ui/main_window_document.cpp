// ui/main_window_document -- the document side of the main window: opening, saving, recent files, the assets root, the title.
//
// Loads and saves run on the job pool (the UI thread only snapshots the version pointer and shows the result); without a pool they
// run inline, which keeps tests simple. A finished job hands its result to the UI thread through a queue drained by a timer.
// Spec: 03_DOCUMENT.md §3, 08_MILESTONES.md T2.2.
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <set>

#include <core/entdef.h>
#include <core/profile.h>
#include <core/textures.h>
#include <core/texplan.h>

#include "base/jobs.h"
#include "base/log.h"
#include "doc/commands.h"
#include "doc/document.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "render/picking.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "scene/builder.h"
#include "scene/point_box.h"
#include "ui/main_window.h"
#include "ui/registry_menus.h"

namespace fs = std::filesystem;

namespace native::ui {

namespace {
constexpr size_t kMaxRecent = 10;

std::string generic(const fs::path& p) { return p.generic_string(); }
} // namespace

bool MainWindow::isDirty() const { return document_ && document_->version != savedVersion_; }

void MainWindow::updateTitle() {
    QString title = QString::fromLatin1(windowTitle());
    if (document_) {
        const QString name = document_->path.empty() ? QStringLiteral("untitled") : QString::fromStdString(document_->path);
        title = name + (isDirty() ? QStringLiteral(" *") : QString()) + QStringLiteral(" - ") + title;
    }
    setWindowTitle(title);
}

void MainWindow::reportError(const QString& message) {
    base::logError(message.toStdString());
    statusBar()->showMessage(message, 8000);
    if (errorHandler_) errorHandler_(message);
    else QMessageBox::critical(this, QString::fromLatin1(windowTitle()), message);
}

void MainWindow::setAssetsRoot(const std::string& folder) {
    assetsRoot_ = folder.empty() ? folder : generic(fs::path(folder).lexically_normal());
    if (services_.settings) services_.settings->setValue("assets/root", QString::fromStdString(assetsRoot_));
    if (prefabLib_) {
        prefabLib_->setAssetsRoot(assetsRoot_);
        updatePrefabs();
    }
    setupScene();
}

MainWindow::ResolvedPath MainWindow::resolveAssetPath(const std::string& path) const {
    ResolvedPath out;
    if (assetsRoot_.empty()) {
        out.error = "Choose the assets folder first (File > Assets folder...).";
        return out;
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(assetsRoot_), ec);
    fs::path p(path);
    if (p.is_relative()) p = root / p;
    const fs::path full = fs::weakly_canonical(p, ec);
    const fs::path rel = full.lexically_relative(root);
    if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) {
        out.error = "\"" + path + "\" is not inside the assets folder " + assetsRoot_;
        return out;
    }
    out.relative = generic(rel);
    out.absolute = full.string();
    return out;
}

void MainWindow::addRecent(const std::string& absolutePath) {
    const std::string key = generic(fs::path(absolutePath));
    recent_.erase(std::remove(recent_.begin(), recent_.end(), key), recent_.end());
    recent_.insert(recent_.begin(), key);
    if (recent_.size() > kMaxRecent) recent_.resize(kMaxRecent);
    if (services_.settings) {
        QStringList list;
        for (const std::string& r : recent_) list << QString::fromStdString(r);
        services_.settings->setValue("files/recent", list);
    }
}

// ---- threading -----------------------------------------------------------------------------------------------------------------

void MainWindow::runJob(std::function<void()> work, std::function<void()> done) {
    ++pendingJobs_;
    if (menus()) menus()->refresh();
    if (!services_.jobs) {
        work();
        --pendingJobs_;
        done();
        return;
    }
    std::shared_ptr<Completed> completed = completed_;
    services_.jobs->submit(
        [work = std::move(work), done = std::move(done), completed]() mutable {
            work();
            std::lock_guard<std::mutex> lock(completed->mutex);
            completed->queue.emplace_back(true, std::move(done));
        },
        /*priority*/ 5);
    completedTimer_.start();
}

void MainWindow::runBackground(std::function<void()> work, std::function<void()> done) {
    if (!services_.jobs) {
        work();
        done();
        return;
    }
    ++backgroundJobs_;
    std::shared_ptr<Completed> completed = completed_;
    services_.jobs->submit(
        [work = std::move(work), done = std::move(done), completed]() mutable {
            work();
            std::lock_guard<std::mutex> lock(completed->mutex);
            completed->queue.emplace_back(false, std::move(done));
        },
        /*priority*/ 3);
    completedTimer_.start();
}

void MainWindow::drainCompleted() {
    for (;;) {
        std::function<void()> next;
        bool counts = true;
        {
            std::lock_guard<std::mutex> lock(completed_->mutex);
            if (completed_->queue.empty()) break;
            counts = completed_->queue.front().first;
            next = std::move(completed_->queue.front().second);
            completed_->queue.pop_front();
        }
        if (counts) --pendingJobs_;
        else --backgroundJobs_;
        next();
    }
    if (pendingJobs_ <= 0) pendingJobs_ = 0;
    if (backgroundJobs_ <= 0) backgroundJobs_ = 0;
    if (pendingJobs_ == 0 && backgroundJobs_ == 0) completedTimer_.stop();
}

// ---- document ------------------------------------------------------------------------------------------------------------------

void MainWindow::setDocument(std::shared_ptr<const doc::DocumentVersion> version, bool saved) {
    if (saved && version) savedVersion_ = version->version;
    history_->reset(std::move(version));   // the listener makes it current, updates the title and menus and tells the panels
}

bool MainWindow::commitEdit(const std::string& label, std::shared_ptr<const doc::DocumentVersion> after) {
    return history_->commit({label, document_, std::move(after)});
}

bool MainWindow::canUndo() const { return history_->canUndo(); }
bool MainWindow::canRedo() const { return history_->canRedo(); }

std::string MainWindow::undoText() const { return history_->canUndo() ? "Undo " + history_->undoLabel() : "Undo"; }
std::string MainWindow::redoText() const { return history_->canRedo() ? "Redo " + history_->redoLabel() : "Redo"; }

void MainWindow::undo() {
    const std::string label = history_->undoLabel();
    if (history_->undo()) statusBar()->showMessage(QStringLiteral("Undo %1").arg(QString::fromStdString(label)), 4000);
}

void MainWindow::redo() {
    const std::string label = history_->redoLabel();
    if (history_->redo()) statusBar()->showMessage(QStringLiteral("Redo %1").arg(QString::fromStdString(label)), 4000);
}

void MainWindow::openDocument(const std::string& path) {
    if (busy()) {
        statusBar()->showMessage(QStringLiteral("Busy: wait for the current load or save to finish"), 4000);
        return;
    }
    const ResolvedPath rp = resolveAssetPath(path);
    if (!rp.error.empty()) {
        reportError(QString::fromStdString(rp.error));
        return;
    }
    const uint64_t serial = ++openSerial_;
    statusBar()->showMessage(QStringLiteral("Loading %1 ...").arg(QString::fromStdString(rp.relative)));
    auto result = std::make_shared<doc::LoadResult>();
    const std::string relative = rp.relative, absolute = rp.absolute;
    runJob([result, relative, absolute] { *result = doc::loadDocumentFile(fs::path(absolute), relative); },
           [this, serial, relative, absolute, result] {
               if (result->version) addRecent(absolute);
               finishOpen(serial, relative, *result);
           });
}

void MainWindow::finishOpen(uint64_t serial, const std::string& relative, const doc::LoadResult& result) {
    if (serial != openSerial_) return;   // a newer open replaced this one
    if (!result.version) {
        reportError(QString::fromStdString(result.error));
        return;
    }
    setDocument(result.version, true);
    const doc::LoadTimings& t = result.timings;
    const QString msg = QStringLiteral("Loaded %1: %2 entities, %3 brushes in %4 ms (read %5, parse %6, nodes %7, validate %8)")
                            .arg(QString::fromStdString(relative))
                            .arg(result.version->entities.size())
                            .arg(result.brushes)
                            .arg(t.totalMs, 0, 'f', 0)
                            .arg(t.readMs, 0, 'f', 0)
                            .arg(t.parseMs, 0, 'f', 0)
                            .arg(t.nodesMs, 0, 'f', 0)
                            .arg(t.validateMs, 0, 'f', 0);
    base::logInfo(msg.toStdString());
    statusBar()->showMessage(msg, 10000);
}

void MainWindow::saveDocument() {
    if (!document_ || busy()) return;
    if (document_->path.empty()) {
        saveAsWithDialog();
        return;
    }
    const ResolvedPath rp = resolveAssetPath(document_->path);
    if (!rp.error.empty()) {
        reportError(QString::fromStdString(rp.error));
        return;
    }
    saveTo(rp.relative, rp.absolute);
}

void MainWindow::saveDocumentAs(const std::string& path) {
    if (!document_ || busy()) return;
    const ResolvedPath rp = resolveAssetPath(path);
    if (!rp.error.empty()) {
        reportError(QString::fromStdString(rp.error));
        return;
    }
    saveTo(rp.relative, rp.absolute);
}

void MainWindow::saveTo(const std::string& relative, const std::string& absolute) {
    // The UI thread only snapshots the version pointer; the worker serialises and writes it.
    const std::shared_ptr<const doc::DocumentVersion> snapshot = document_;
    statusBar()->showMessage(QStringLiteral("Saving %1 ...").arg(QString::fromStdString(relative)));
    auto error = std::make_shared<std::string>();
    runJob([snapshot, absolute, error] { *error = doc::saveDocumentFile(*snapshot, fs::path(absolute)); },
           [this, snapshot, relative, absolute, error] {
               if (!error->empty()) {
                   reportError(QString::fromStdString(*error));
                   return;
               }
               addRecent(absolute);
               if (document_ && document_->path != relative) {
                   // Save As: the document takes the new path (a new version over the same nodes).
                   auto renamed = std::make_shared<doc::DocumentVersion>(*document_);
                   renamed->path = relative;
                   renamed->version = doc::nextVersionNumber();
                   const bool wasSnapshot = document_ == snapshot;
                   if (wasSnapshot) savedVersion_ = renamed->version;
                   history_->reset(renamed);   // versions carry their path: the undo steps of the old path end here
               } else if (document_ == snapshot) {
                   savedVersion_ = snapshot->version;
               }
               updateTitle();
               if (menus()) menus()->refresh();
               emit documentChanged();
               statusBar()->showMessage(QStringLiteral("Saved %1").arg(QString::fromStdString(relative)), 6000);
           });
}

// ---- prefabs -------------------------------------------------------------------------------------------------------------------

void MainWindow::updatePrefabs() {
    if (!document_) {
        ++expandSerial_;
        if (expansion_) {
            expansion_.reset();
            emit prefabExpansionChanged();
        }
        return;
    }
    if (document_->instances.empty() && (!expansion_ || expansion_->instances.empty())) {
        if (!expansion_ || expansion_->documentVersion != document_->version) {
            auto empty = std::make_shared<doc::Expansion>();
            empty->documentVersion = document_->version;
            expansion_ = empty;
            emit prefabExpansionChanged();
        }
        return;
    }
    const uint64_t serial = ++expandSerial_;
    const auto version = document_;
    const auto resolver = prefabResolver_;
    const auto library = prefabLib_;
    auto result = std::make_shared<std::shared_ptr<const doc::Expansion>>();
    runBackground([version, resolver, library, result] { *result = resolver->expand(*version); },
                  [this, serial, result] {
                      if (serial != expandSerial_) return;   // a newer document version asked again
                      expansion_ = *result;
                      emit prefabExpansionChanged();
                      // Problems go to the log, not the status bar: they would hide the "Loaded ..." message of the open that caused them.
                      for (const std::string& problem : expansion_->problems()) base::logWarning("prefab " + problem);
                  });
}

void MainWindow::pollPrefabs() {
    if (!document_ || prefabPolling_ || prefabLib_->empty()) return;
    prefabPolling_ = true;
    const auto version = document_;
    const auto resolver = prefabResolver_;
    const auto library = prefabLib_;
    struct Found {
        std::vector<std::string> changed;
        std::shared_ptr<const doc::Expansion> expansion;
    };
    auto found = std::make_shared<Found>();
    runBackground(
        [version, resolver, library, found] {
            found->changed = library->refresh();
            if (!found->changed.empty()) found->expansion = resolver->expand(*version);
        },
        [this, version, found] {
            prefabPolling_ = false;
            if (found->changed.empty()) return;
            if (document_ != version) {   // the document moved on meanwhile: its own expansion picks the reloaded files up
                updatePrefabs();
                return;
            }
            // Instances that read a changed file, before and after (a file can start or stop being needed when an include changed).
            std::set<std::string> ids;
            if (expansion_)
                for (const std::string& id : expansion_->instancesUsing(found->changed)) ids.insert(id);
            for (const std::string& id : found->expansion->instancesUsing(found->changed)) ids.insert(id);
            doc::ChangeSet cs;
            cs.fromVersion = cs.toVersion = version->version;
            for (const std::string& id : ids) {
                doc::ChangedEntity ce;
                ce.instance = id;
                ce.kinds = doc::Prefab;
                for (const auto& n : version->instances)
                    if (n->typed.id == id) {
                        ce.before.add(n->typed.origin);
                        ce.after.add(n->typed.origin);
                    }
                cs.entities.push_back(std::move(ce));
            }
            expansion_ = found->expansion;
            emit prefabExpansionChanged();
            emit prefabsChanged(cs);
            const std::vector<std::string> problems = expansion_->problems();
            statusBar()->showMessage(
                problems.empty() ? QStringLiteral("Prefab %1 changed on disk: %2 instance(s) updated")
                                       .arg(QString::fromStdString(found->changed.front()))
                                       .arg(static_cast<int>(ids.size()))
                                 : QString::fromStdString("Prefabs: " + problems.front()),
                8000);
        });
}

// ---- dialogs -------------------------------------------------------------------------------------------------------------------

void MainWindow::chooseAssetsRootWithDialog() {
    const QString start = assetsRoot_.empty() ? QDir::homePath() : QString::fromStdString(assetsRoot_);
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Assets folder"), start);
    if (!dir.isEmpty()) setAssetsRoot(dir.toStdString());
}

void MainWindow::openWithDialog() {
    if (assetsRoot_.empty()) {
        chooseAssetsRootWithDialog();
        if (assetsRoot_.empty()) return;
    }
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Open map"), QString::fromStdString(assetsRoot_ + "/maps"),
                                                      QStringLiteral("Maps (*.map.json);;All files (*)"));
    if (!file.isEmpty()) openDocument(file.toStdString());
}

void MainWindow::saveAsWithDialog() {
    if (!document_) return;
    if (assetsRoot_.empty()) {
        chooseAssetsRootWithDialog();
        if (assetsRoot_.empty()) return;
    }
    const std::string startRel = document_->path.empty() ? "maps/untitled.map.json" : document_->path;
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save map as"),
                                                      QString::fromStdString(assetsRoot_ + "/" + startRel),
                                                      QStringLiteral("Maps (*.map.json);;All files (*)"));
    if (!file.isEmpty()) saveDocumentAs(file.toStdString());
}


// ---- the scene (T3.4) ----------------------------------------------------------------------------------------------------------

void MainWindow::setupScene() {
    if (!services_.renderThread || !services_.jobs) return;
    if (!builder_) {
        builder_ = std::make_unique<scene::SceneBuilder>(*services_.jobs);
        render::RenderThread* rt = services_.renderThread;
        builder_->setListener([this, rt](const std::shared_ptr<const scene::SceneSnapshot>& snapshot) {
            rt->setSnapshot(snapshot);
            // picking and the outline live on the UI thread
            QMetaObject::invokeMethod(this, [this, snapshot] { onSnapshot(snapshot); }, Qt::QueuedConnection);
        });
    }
    if (assetsRoot_.empty()) return;
    const fs::path root(assetsRoot_);
    const core::GameProfile profile = core::loadProfile(root);
    texEnv_ = std::make_shared<core::texplan::TexEnvironment>(root, profile);
    scene::ReliefEnv env;
    const auto e = texEnv_;
    env.info = [e](const std::string& p) { return e->info(p); };
    env.groups = [e](const std::string& n) { return e->group(n); };
    env.revision = ++reliefRevision_;
    builder_->setReliefEnv(env);
    services_.renderThread->setTextureSource(root, services_.jobs);
    // the game's entity definitions: class boxes (picking, drawing), colours
    entityDefs_ = std::make_shared<core::EntityDefinitions>(core::loadEntityDefinitions(root / profile.entityDefinitions));
    if (!picker_) picker_ = std::make_unique<render::Picker>();
    picker_->setBoxOf(scene::classBoxes(entityDefs_.get()));
    prefabRoot_ = profile.prefabRoot;
    refreshOverlays();
    emit entityDefinitionsChanged();
}

void MainWindow::submitScene(const doc::ChangeSet& changes) {
    if (!builder_ || !document_) return;
    builder_->submit(document_, changes, expansion_);
}

// The level's sky as the Shell picks it (model/sky.ts levelSkyTexture): `_sky_texture`, else with Auto sky on the first skybox of the assets.
void MainWindow::updateSky() {
    if (!services_.renderThread) return;
    std::string sky;
    if (document_) {
        for (const auto& e : document_->entities) {
            if (e->typed.classname != "worldspawn") continue;
            const auto& kv = e->typed.keyvalues;
            const auto t = kv.find("_sky_texture");
            if (t != kv.end() && !t->second.empty()) {
                sky = t->second;
            } else {
                const auto a = kv.find("_sky_auto");
                if (a != kv.end() && (a->second == "box" || a->second == "areas") && !assetsRoot_.empty()) {
                    const auto boxes = core::listSkyboxes(fs::path(assetsRoot_));
                    if (!boxes.empty()) sky = boxes.front();
                }
            }
            break;
        }
    }
    services_.renderThread->setSkyTexture(sky);
}

bool MainWindow::sceneReady() const {
    if (!builder_ || !services_.renderThread || !builder_->idle() || !builder_->snapshot()) return false;
    const render::RenderThread::SceneInfo info = services_.renderThread->sceneInfo();
    return info.scene.queued == 0 && info.texturesPending == 0 && info.scene.objects > 0;
}

bool MainWindow::frameScene() {
    const auto snap = builder_ ? builder_->snapshot() : nullptr;
    if (!snap) return false;
    doc::Box box;
    for (const auto& [key, p] : snap->objects)
        if (p.additive && p.bounds.valid()) box.add(p.bounds);
    if (!box.valid()) return false;
    for (const ViewportPanel& panel : panels_) {
        panel.window->rig().frame({box.mn.x, box.mn.y, box.mn.z}, {box.mx.x, box.mx.y, box.mx.z});
        panel.window->viewChanged();
    }
    return true;
}

} // namespace native::ui
