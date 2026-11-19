// ui/stats_panel -- the Stats dock panel: device, driver, API version and every DeviceCaps flag.
//
// Owns: a read-only two-column table. Later tasks add frame / GPU timings (02_BUILD_AND_TOOLING.md §8). Thread: UI thread.
// Spec: 08_MILESTONES.md T1.2.
#pragma once

#include <QTableWidget>

#include <string>
#include <utility>
#include <vector>

namespace native::ui {

class StatsPanel : public QTableWidget {
public:
    explicit StatsPanel(QWidget* parent = nullptr);

    // Replaces the content with name / value rows (DeviceCaps::rows()).
    void setRows(const std::vector<std::pair<std::string, std::string>>& rows);
};

} // namespace native::ui
