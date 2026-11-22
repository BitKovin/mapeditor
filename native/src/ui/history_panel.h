// ui/history_panel -- the History dock (10_UI_AND_INPUT.md U3): the undo history as a list, the current state marked, the redo steps after it in grey; a click jumps to that
// state in one step (doc::History::goTo). It follows commits, undo / redo and new documents (rebuilt a moment after the last change so a drag does not rebuild it per move).
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U3), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QWidget>

class QListWidget;
class QTimer;

namespace native::ui {

class MainWindow;

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Rebuilds the list now (what the timer does a moment after a change).
    void refreshNow();
    /// The rows as shown: "Start" first, then one per command; the current row is `currentRow()`.
    QStringList rows() const;
    int currentRow() const;
    /// Jumps to the state of a row (what a click does); false when it is the current one or out of range.
    bool jumpTo(int row);

private:
    MainWindow& win_;
    QListWidget* list_ = nullptr;
    QTimer* timer_ = nullptr;
    bool filling_ = false;
};

} // namespace native::ui
