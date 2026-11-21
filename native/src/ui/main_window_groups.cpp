// ui/main_window_groups -- MainWindow: texture groups (T11.4b): the group files through Core, applying a group to the selected faces, and the group editor window.
#include <QStatusBar>

#include "core/texgroups.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"

namespace native::ui {

std::vector<std::string> MainWindow::texGroupList() const {
    if (groupStore_.list) return groupStore_.list();
    if (!profile_ || assetsRoot_.empty()) return {};
    return core::listTexGroups(std::filesystem::path(assetsRoot_), *profile_);
}

std::optional<nlohmann::json> MainWindow::texGroupRead(const std::string& name) const {
    if (groupStore_.read) return groupStore_.read(name);
    if (!profile_ || assetsRoot_.empty()) return std::nullopt;
    return core::readTexGroup(std::filesystem::path(assetsRoot_), *profile_, name);
}

std::string MainWindow::texGroupWrite(const std::string& name, const nlohmann::json& group) {
    std::string err;
    if (groupStore_.write) err = groupStore_.write(name, group);
    else if (!profile_ || assetsRoot_.empty()) err = "no game folder";
    else err = core::writeTexGroup(std::filesystem::path(assetsRoot_), *profile_, name, group);
    if (!err.empty()) {
        statusBar()->showMessage(QString::fromStdString("Texture group: " + err), 6000);
        return err;
    }
    rebuildTextureEnvironment();
    return {};
}

std::string MainWindow::texGroupRemove(const std::string& name) {
    std::string err;
    if (groupStore_.remove) err = groupStore_.remove(name);
    else if (!profile_ || assetsRoot_.empty()) err = "no game folder";
    else err = core::deleteTexGroup(std::filesystem::path(assetsRoot_), *profile_, name);
    if (!err.empty()) {
        statusBar()->showMessage(QString::fromStdString("Texture group: " + err), 6000);
        return err;
    }
    rebuildTextureEnvironment();
    return {};
}

bool MainWindow::applyTexGroup(const std::string& name) {
    if (!document_) return false;
    std::string fallback;
    bool wallsOnly = false;
    if (const auto raw = texGroupRead(name)) {
        wallsOnly = raw->contains("kit") && (*raw)["kit"].is_object() && (*raw)["kit"].contains("kind") && (*raw)["kit"]["kind"] == "wall";
        const core::texplan::GroupParse parse = core::texplan::parseGroup(nlohmann::ordered_json::parse(raw->dump()));
        if (parse.group)
            if (const auto fb = core::texplan::groupFallbackTexture(*parse.group)) fallback = *fb;
    }
    const tools::BrushEdit e = tools::tex::applyTexGroup(document_, selection_->items(), name, fallback, wallsOnly);
    if (!e.ok() && e.message.empty()) {
        statusBar()->showMessage(QStringLiteral("Select a brush or a face to apply the group to"), 5000);
        return false;
    }
    return applyBrushEdit(e);
}

TexGroupEditor* MainWindow::openTexGroupEditor(const std::string& name, bool show) {
    if (groupEditor_) {
        groupEditor_->close();
        groupEditor_->deleteLater();
        groupEditor_ = nullptr;
    }
    groupEditor_ = new TexGroupEditor(*this, name);
    QObject::connect(groupEditor_, &QObject::destroyed, this, [this](QObject* o) {
        if (o == groupEditor_) groupEditor_ = nullptr;
    });
    if (show) {
        groupEditor_->show();
        groupEditor_->raise();
    }
    return groupEditor_;
}

} // namespace native::ui
