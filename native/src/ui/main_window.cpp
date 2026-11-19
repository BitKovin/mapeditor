// ui/main_window -- see main_window.h.
#include "ui/main_window.h"

#include <QDockWidget>
#include <QWidget>

#include "render/viewport_window.h"
#include "ui/stats_panel.h"

namespace native::ui {

const char* MainWindow::windowTitle() { return "Map Editor (native)"; }

MainWindow::MainWindow(const Services& services, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromLatin1(windowTitle()));
    resize(1280, 800);

    if (services.renderThread && services.vulkanInstance) {
        viewport_ = new render::ViewportWindow(*services.renderThread, *services.vulkanInstance);
        viewportContainer_ = QWidget::createWindowContainer(viewport_, this);   // the container owns the window
        viewportContainer_->setMinimumSize(160, 120);
        viewportContainer_->setFocusPolicy(Qt::StrongFocus);
        viewportDock_ = new QDockWidget(QStringLiteral("Viewport"), this);
        viewportDock_->setObjectName(QStringLiteral("viewportDock"));
        viewportDock_->setWidget(viewportContainer_);
        addDockWidget(Qt::LeftDockWidgetArea, viewportDock_);
    }

    auto* statsDock = new QDockWidget(QStringLiteral("Stats"), this);
    statsDock->setObjectName(QStringLiteral("statsDock"));
    stats_ = new StatsPanel(statsDock);
    statsDock->setWidget(stats_);
    addDockWidget(Qt::RightDockWidgetArea, statsDock);
    resizeDocks({statsDock}, {440}, Qt::Horizontal);
    if (!viewportDock_) setCentralWidget(new QWidget(this));   // widget-only tests: keep the stats dock from filling the window
}

void MainWindow::setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows) { stats_->setRows(rows); }

} // namespace native::ui
