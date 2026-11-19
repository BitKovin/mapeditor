// ui/stats_panel -- see stats_panel.h.
#include "ui/stats_panel.h"

#include <QHeaderView>
#include <QTableWidgetItem>

namespace native::ui {

StatsPanel::StatsPanel(QWidget* parent) : QTableWidget(0, 2, parent) {
    setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    verticalHeader()->hide();
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::NoSelection);
    setShowGrid(false);
    setAlternatingRowColors(true);
    setWordWrap(false);
    setFocusPolicy(Qt::NoFocus);
    viewport()->setAttribute(Qt::WA_Hover, false);   // no hover highlight: the panel is read-only
    verticalHeader()->setDefaultSectionSize(24);
}

void StatsPanel::setRows(const std::vector<std::pair<std::string, std::string>>& rows) {
    setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        setItem(i, 0, new QTableWidgetItem(QString::fromStdString(rows[static_cast<size_t>(i)].first)));
        auto* value = new QTableWidgetItem(QString::fromStdString(rows[static_cast<size_t>(i)].second));
        const QString v = value->text();
        if (v == QStringLiteral("on")) value->setForeground(QColor(0x55, 0xc2, 0x6a));
        else if (v == QStringLiteral("off")) value->setForeground(QColor(0xe0, 0x7a, 0x5f));
        setItem(i, 1, value);
    }
}

} // namespace native::ui
