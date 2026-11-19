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

#include "base/jobs.h"
#include "base/log.h"
#include "doc/document.h"
#include "doc/io.h"
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
            completed->queue.push_back(std::move(done));
        },
        /*priority*/ 5);
    completedTimer_.start();
}

void MainWindow::drainCompleted() {
    for (;;) {
        std::function<void()> next;
        {
            std::lock_guard<std::mutex> lock(completed_->mutex);
            if (completed_->queue.empty()) break;
            next = std::move(completed_->queue.front());
            completed_->queue.pop_front();
        }
        --pendingJobs_;
        next();
    }
    if (pendingJobs_ <= 0) {
        pendingJobs_ = 0;
        completedTimer_.stop();
    }
}

// ---- document ------------------------------------------------------------------------------------------------------------------

void MainWindow::setDocument(std::shared_ptr<const doc::DocumentVersion> version, bool saved) {
    document_ = std::move(version);
    if (saved && document_) savedVersion_ = document_->version;
    updateTitle();
    if (menus()) menus()->refresh();
    emit documentChanged();
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
                   document_ = renamed;
                   if (wasSnapshot) savedVersion_ = renamed->version;
               } else if (document_ == snapshot) {
                   savedVersion_ = snapshot->version;
               }
               updateTitle();
               if (menus()) menus()->refresh();
               emit documentChanged();
               statusBar()->showMessage(QStringLiteral("Saved %1").arg(QString::fromStdString(relative)), 6000);
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

} // namespace native::ui
