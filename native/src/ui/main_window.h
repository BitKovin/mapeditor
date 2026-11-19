// ui/main_window -- the application's top-level window.
//
// Owns: the QMainWindow shell: title and the Stats dock (T1.2). Geometry persistence and the Qt ADS dock manager arrive with
// T1.4 (the Stats dock moves into it), menus built from the action registry with T1.6.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md group C (app/main_window), 10_UI_AND_INPUT.md.
#pragma once

#include <QMainWindow>

#include <string>
#include <utility>
#include <vector>

namespace native::ui {

class StatsPanel;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

    static const char* windowTitle();

    // Fills the Stats dock (gpu::DeviceCaps::rows()).
    void setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows);

private:
    StatsPanel* stats_ = nullptr;
};

} // namespace native::ui
