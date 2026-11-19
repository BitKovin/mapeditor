// ui/main_window -- see main_window.h.
#include "ui/main_window.h"

#include <QDockWidget>
#include <QWidget>

#include "ui/stats_panel.h"

namespace native::ui {

const char* MainWindow::windowTitle() { return "Map Editor (native)"; }

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromLatin1(windowTitle()));
    resize(1280, 800);

    auto* dock = new QDockWidget(QStringLiteral("Stats"), this);
    dock->setObjectName(QStringLiteral("statsDock"));
    stats_ = new StatsPanel(dock);
    dock->setWidget(stats_);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    // Empty centre (the viewports arrive with T1.3); the dock keeps a fixed-ish side width.
    setCentralWidget(new QWidget(this));
    resizeDocks({dock}, {440}, Qt::Horizontal);
}

void MainWindow::setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows) { stats_->setRows(rows); }

} // namespace native::ui
