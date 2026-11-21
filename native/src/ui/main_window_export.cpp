// ui/main_window_export -- File > Export, Play map and Import Quake Map (T9.3). The export itself is exporting/export_job (the Core's exporter on its own thread);
// this file is the dialogs, the status bar progress, starting the game and replacing the map by an imported one.
// Thread: UI thread. Spec: 08_MILESTONES.md T9.3, 06_LIGHTING.md section 13.
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <filesystem>

#include "base/jobs.h"
#include "base/log.h"
#include "doc/document.h"
#include "exporting/export_job.h"
#include "exporting/import_map.h"
#include "ui/main_window.h"

namespace native::ui {

namespace fs = std::filesystem;

namespace {

QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }

// The settings key part of a game folder: its path without separators.
QString folderKey(const std::string& root) {
    QString k = qs(root);
    for (QChar& c : k)
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')) c = QLatin1Char('|');
    return k;
}

} // namespace

bool MainWindow::exporting() const { return exportJob_ && exportJob_->running(); }

exporting::LaunchSettings MainWindow::launchSettings() const {
    exporting::LaunchSettings s;
    QSettings* st = settings();
    if (!st || assetsRoot_.empty()) return s;
    const QString base = QStringLiteral("game/") + folderKey(assetsRoot_) + QLatin1Char('/');
    s.exe = st->value(base + QStringLiteral("exe")).toString().toUtf8().toStdString();
    s.args = st->value(base + QStringLiteral("args"), qs(s.args)).toString().toUtf8().toStdString();
    s.workingDir = st->value(base + QStringLiteral("workingDir")).toString().toUtf8().toStdString();
    return s;
}

void MainWindow::setLaunchSettings(const exporting::LaunchSettings& s) {
    QSettings* st = settings();
    if (!st || assetsRoot_.empty()) return;
    const QString base = QStringLiteral("game/") + folderKey(assetsRoot_) + QLatin1Char('/');
    st->setValue(base + QStringLiteral("exe"), qs(s.exe));
    st->setValue(base + QStringLiteral("args"), qs(s.args));
    st->setValue(base + QStringLiteral("workingDir"), qs(s.workingDir));
}

void MainWindow::exportWithDialog() {
    if (!document_) return;
    if (exporting()) {
        cancelExport();
        return;
    }
    if (assetsRoot_.empty()) {
        chooseAssetsRootWithDialog();
        if (assetsRoot_.empty()) return;
    }
    exporting::ExporterInfo info;
    try {
        info = exporting::exporterOf(fs::path(assetsRoot_));
    } catch (const std::exception& e) {
        reportError(QStringLiteral("game_profile.json: ") + QString::fromUtf8(e.what()));
        return;
    }
    const fs::path map = document_->path.empty() ? fs::path("maps/untitled") : fs::path(document_->path);
    std::string base = map.filename().string();
    if (const size_t dot = base.find('.'); dot != std::string::npos) base.resize(dot);   // "lvl2.map.json" -> "lvl2"
    const fs::path suggested = fs::path(assetsRoot_) / map.parent_path() / (base + info.extension);
    const QString filter = QStringLiteral("%1 (*%2);;All files (*)").arg(qs(info.name), qs(info.extension));
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Export %1").arg(qs(info.name)), qs(suggested.generic_string()), filter);
    if (!file.isEmpty()) startExport(file.toUtf8().toStdString(), false);
}

bool MainWindow::startExport(const std::string& path, bool play, bool bake) {
    if (!document_) return false;
    if (exporting()) {
        statusBar()->showMessage(QStringLiteral("An export is already running"), 4000);
        return false;
    }
    const ResolvedPath rp = resolveAssetPath(path);
    if (!rp.error.empty()) {
        reportError(qs(rp.error));
        return false;
    }
    exporting::ExportOptions options;
    options.assetsRoot = fs::path(assetsRoot_);
    options.outPath = rp.relative;
    options.bake = bake;
    core::ExportRequest request;
    std::string warning;
    try {
        request = exporting::makeExportRequest(*document_, options, &warning);
    } catch (const std::exception& e) {
        reportError(QStringLiteral("Export: ") + QString::fromUtf8(e.what()));
        return false;
    }
    if (!warning.empty()) base::logInfo("export: " + warning);
    if (!exportJob_) exportJob_ = std::make_shared<exporting::ExportJob>();
    if (!exportTimer_) {
        exportTimer_ = new QTimer(this);
        exportTimer_->setInterval(100);
        QObject::connect(exportTimer_, &QTimer::timeout, this, [this] { finishExport(); });
    }
    playAfterExport_ = play;
    exportOut_ = request.outPath;
    if (!exportJob_->start(std::move(request))) return false;
    statusBar()->showMessage(QStringLiteral("Exporting %1 ...").arg(qs(exportOut_)));
    exportTimer_->start();
    return true;
}

void MainWindow::finishExport() {
    if (!exportJob_) return;
    if (exportJob_->running()) {
        const auto p = exportJob_->progress();
        statusBar()->showMessage(QStringLiteral("Exporting %1: %2 (%3 %)").arg(qs(exportOut_), qs(p.stage)).arg(int(p.fraction * 100)));
        return;
    }
    exportTimer_->stop();
    const auto result = exportJob_->take();
    if (!result) return;
    lastExport_ = std::make_shared<core::BspExportResult>(*result);
    for (const std::string& w : result->warnings) base::logInfo("export warning: " + w);
    if (result->cancelled) {
        statusBar()->showMessage(QStringLiteral("Export cancelled"), 6000);
        return;
    }
    if (!result->ok) {
        reportError(QStringLiteral("Export failed: ") + qs(result->error));
        return;
    }
    QString msg = QStringLiteral("Exported %1 in %2 s (%3 surfaces").arg(qs(result->bspPath)).arg(result->seconds, 0, 'f', 1).arg(result->stats.surfaces);
    if (!result->warnings.empty()) msg += QStringLiteral(", %1 warnings, see the log").arg(result->warnings.size());
    msg += QLatin1Char(')');
    statusBar()->showMessage(msg, 15000);
    base::logInfo(msg.toStdString());
    if (playAfterExport_) launchGame(result->bspPath);
}

void MainWindow::cancelExport() {
    if (!exporting()) return;
    exportJob_->cancel();
    statusBar()->showMessage(QStringLiteral("Cancelling the export ..."));
}

void MainWindow::playMap() {
    if (!document_) return;
    if (exporting()) {
        statusBar()->showMessage(QStringLiteral("An export is already running"), 4000);
        return;
    }
    if (document_->path.empty()) {
        reportError(QStringLiteral("Play map: save the map first (the game is started with the file next to it)."));
        return;
    }
    const exporting::LaunchSettings launch = launchSettings();
    if (launch.exe.empty()) {
        reportError(QStringLiteral("Play map: set the game's executable in Settings > General > Run game."));
        return;
    }
    exporting::ExporterInfo info;
    try {
        info = exporting::exporterOf(fs::path(assetsRoot_));
    } catch (const std::exception& e) {
        reportError(QStringLiteral("game_profile.json: ") + QString::fromUtf8(e.what()));
        return;
    }
    const fs::path map(document_->path);
    std::string base = map.filename().string();
    if (const size_t dot = base.find('.'); dot != std::string::npos) base.resize(dot);
    startExport((map.parent_path() / (base + info.extension)).generic_string(), true);
}

void MainWindow::launchGame(const std::string& bspRelative) {
    const exporting::LaunchCommand c = exporting::launchCommand(launchSettings(), fs::path(assetsRoot_), bspRelative);
    QStringList args;
    for (const std::string& a : c.args) args << qs(a);
    if (!QFileInfo::exists(qs(c.exe))) {
        reportError(QStringLiteral("Play map: the game executable does not exist: %1").arg(qs(c.exe)));
        return;
    }
    if (!QProcess::startDetached(qs(c.exe), args, qs(c.workingDir))) {
        reportError(QStringLiteral("Play map: could not start %1").arg(qs(c.exe)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Started %1 %2").arg(qs(c.exe), args.join(QLatin1Char(' '))), 8000);
}

void MainWindow::importQuakeMapWithDialog() {
    if (busy()) return;
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import Quake map"), QString(), QStringLiteral("Quake maps (*.map);;All files (*)"));
    if (!file.isEmpty()) importQuakeMap(file.toUtf8().toStdString());
}

bool MainWindow::importQuakeMap(const std::string& file) {
    if (busy()) return false;
    if (isDirty() && !confirm(QStringLiteral("Import Quake map"), QStringLiteral("The map has unsaved changes. Discard them?"))) return false;
    statusBar()->showMessage(QStringLiteral("Importing %1 ...").arg(qs(file)));
    auto result = std::make_shared<exporting::ImportedMap>();
    const std::string root = assetsRoot_;
    runJob([result, file, root] { *result = exporting::importQuakeMap(fs::path(file), fs::path(root)); },
           [this, result, file] {
               if (!result->version) {
                   reportError(QStringLiteral("Import failed: ") + qs(result->error));
                   return;
               }
               for (const std::string& w : result->warnings) base::logInfo("import warning: " + w);
               setDocument(result->version, false);   // not on disk: Save asks where to put it
               statusBar()->showMessage(QStringLiteral("Imported %1: %2 entities, %3 brushes, %4 patches, %5 lights; %6 skipped, %7 warnings%8")
                                            .arg(qs(file)).arg(result->entities).arg(result->brushes).arg(result->patches).arg(result->lights).arg(result->skipped)
                                            .arg(result->warnings.size()).arg(result->warnings.empty() ? QString() : QStringLiteral(" (see the log)")),
                                        15000);
           });
    return true;
}

} // namespace native::ui
