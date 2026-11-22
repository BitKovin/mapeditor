// ui/main_window_bookmarks -- camera bookmarks (10_UI_AND_INPUT.md U7): nine views per map, kept in the user settings under the map's path, never in the map. A bookmark is the
// camera mode and the poses of the active viewport.
#include <QSettings>
#include <QStatusBar>

#include <nlohmann/json.hpp>

#include "render/viewport_window.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

// "bookmarks/<map path with / and \ turned into |>/<slot>"; an unsaved map has the name "(unsaved)".
std::string keyFor(const std::string& mapPath, int slot) {
    std::string p = mapPath.empty() ? "(unsaved)" : mapPath;
    for (char& c : p)
        if (c == '/' || c == '\\') c = '|';
    return "bookmarks/" + p + "/" + std::to_string(slot);
}

} // namespace

bool MainWindow::hasCameraBookmark(int slot) const {
    if (!document_ || slot < 1 || slot > 9) return false;
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) return st->contains(QString::fromStdString(key));
    return bookmarks_.count(key) != 0;
}

bool MainWindow::saveCameraBookmark(int slot) {
    render::ViewportWindow* vp = activeViewport();
    if (!vp || !document_ || slot < 1 || slot > 9) return false;
    const nlohmann::json j = {{"mode", static_cast<int>(vp->rig().mode())}, {"pose", vp->rig().poseToJson()}};
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) st->setValue(QString::fromStdString(key), QString::fromStdString(j.dump()));
    else bookmarks_[key] = j.dump();
    statusBar()->showMessage(QStringLiteral("Camera bookmark %1 saved").arg(slot), 3000);
    return true;
}

bool MainWindow::recallCameraBookmark(int slot) {
    render::ViewportWindow* vp = activeViewport();
    if (!vp || !document_ || slot < 1 || slot > 9) return false;
    std::string text;
    const std::string key = keyFor(document_->path, slot);
    if (QSettings* st = services_.settings) text = st->value(QString::fromStdString(key)).toString().toStdString();
    else if (auto it = bookmarks_.find(key); it != bookmarks_.end()) text = it->second;
    if (text.empty()) {
        statusBar()->showMessage(QStringLiteral("Camera bookmark %1 is empty").arg(slot), 3000);
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("pose") || !j["pose"].is_string()) return false;
    const int mode = j.value("mode", 0);
    if (mode >= 0 && mode <= static_cast<int>(render::CameraMode::Side)) vp->setCameraMode(static_cast<render::CameraMode>(mode));
    vp->rig().poseFromJson(j["pose"].get<std::string>());
    vp->viewChanged();
    statusBar()->showMessage(QStringLiteral("Camera bookmark %1").arg(slot), 3000);
    return true;
}

} // namespace native::ui
