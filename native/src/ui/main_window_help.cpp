// ui/main_window_help -- MainWindow: running actions by id, the command palette, the status bar hint source and the key help sheet (T4.7).
#include <QSettings>

#include "ui/command_palette.h"
#include "ui/key_help_overlay.h"
#include "ui/main_window.h"
#include "ui/transform_session.h"

#include "render/viewport_window.h"

namespace native::ui {

bool MainWindow::runAction(const std::string& id, const nlohmann::json& params) {
    UiEnv ui = currentUiEnv();
    actions::ActionEnv env{&ui};
    return registry_.run(id, env, params);
}

void MainWindow::openPalette() {
    if (findChild<CommandPalette*>(QStringLiteral("palette"))) return;   // already open
    CommandPalette palette(*this);
    if (palette.exec() == QDialog::Accepted) palette.runChosen();
}

std::vector<actions::Recent> MainWindow::paletteRecent() const {
    if (!services_.settings) return {};
    return actions::recentFromJson(services_.settings->value("palette/recent").toString().toStdString());
}

void MainWindow::setPaletteRecent(const std::vector<actions::Recent>& list) {
    if (!services_.settings) return;
    services_.settings->setValue("palette/recent", QString::fromStdString(actions::recentToJson(list)));
}

std::string MainWindow::modeContext() const {
    if (transform_ && transform_->source == TransformSession::Source::Modal) return "modal:transform";
    return tool_ == Tool::Box ? "tool:box" : "tool:select";
}

std::string MainWindow::hintText() const { return actions::hintFor(registry_, keymap_, modeContext()); }

bool MainWindow::keyHelpShown() const { return keyHelp_ && keyHelp_->isVisible(); }

void MainWindow::showKeyHelp(bool show) {
    if (!show) {
        if (keyHelp_) keyHelp_->hide();
        return;
    }
    if (!keyHelp_) {
        keyHelp_ = new KeyHelpOverlay(this);
        QObject::connect(this, &MainWindow::keymapChanged, keyHelp_, [this] {
            if (keyHelp_->isVisible()) keyHelp_->setSections(actions::keyHelpSections(registry_, keymap_, router_.activeContexts()));
        });
    }
    keyHelp_->setSections(actions::keyHelpSections(registry_, keymap_, router_.activeContexts()));
    QRect area = frameGeometry();
    if (render::ViewportWindow* vp = activeViewport()) {
        for (const ViewportPanel& p : panels_)
            if (p.window == vp) area = QRect(p.container->mapToGlobal(QPoint(0, 0)), p.container->size());
    }
    keyHelp_->showOver(area);
}

} // namespace native::ui
