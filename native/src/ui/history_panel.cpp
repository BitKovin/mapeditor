// ui/history_panel -- see history_panel.h.
#include "ui/history_panel.h"

#include <QBrush>
#include <QListWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "doc/commands.h"
#include "ui/main_window.h"

namespace native::ui {

HistoryPanel::HistoryPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("HistoryPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("historyList"));
    list_->setToolTip(QStringLiteral("The undo history. Click a step to go back or forward to it; a new edit drops the grey steps."));
    v->addWidget(list_);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(40);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    const auto later = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::documentChanged, this, later);
    QObject::connect(&win_, &MainWindow::documentEdited, this, later);
    QObject::connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!filling_ && row >= 0) jumpTo(row);
    });
    // not refreshed here: the window makes its History after its panels (it calls refreshNow then)
}

void HistoryPanel::refreshNow() {
    timer_->stop();
    const doc::History& h = win_.history();
    const std::vector<std::string> labels = h.labels();
    const int current = static_cast<int>(h.position());
    filling_ = true;
    list_->clear();
    list_->addItem(QStringLiteral("Start"));
    for (const std::string& l : labels) list_->addItem(QString::fromStdString(l));
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        if (i > current) {
            it->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
            QFont f = it->font();
            f.setItalic(true);
            it->setFont(f);
        }
    }
    list_->setCurrentRow(current);
    filling_ = false;
}

QStringList HistoryPanel::rows() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->text();
    return out;
}

int HistoryPanel::currentRow() const { return list_->currentRow(); }

bool HistoryPanel::jumpTo(int row) {
    if (row < 0) return false;
    const bool ok = win_.history().goTo(static_cast<size_t>(row));
    if (ok) win_.statusBar()->showMessage(QStringLiteral("History: %1").arg(list_->item(row) ? list_->item(row)->text() : QString()), 3000);
    return ok;
}

} // namespace native::ui
