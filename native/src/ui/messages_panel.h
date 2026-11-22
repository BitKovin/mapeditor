// ui/messages_panel -- the Messages dock (10_UI_AND_INPUT.md U14): the history of the status bar's messages ("Selected 3 items", "Clipper: ...", export results, refused edits), with the
// time, so a message that went away after a few seconds can still be read. Non-blocking messages are not dialogs; this is where they stay.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U14), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QDateTime>
#include <QWidget>

class QListWidget;

namespace native::ui {

class MainWindow;

class MessagesPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxMessages = 200;
    // A message that follows the previous one within this time replaces it (a live reading that changes with the mouse is one entry, not hundreds).
    static constexpr qint64 kMergeMs = 300;

    explicit MessagesPanel(MainWindow& window, QWidget* parent = nullptr);

    /// Adds a message (the status bar's messageChanged calls this; empty texts and a repeat of the last text are ignored).
    void add(const QString& text, const QDateTime& when = QDateTime::currentDateTime());
    void clear();
    /// "12:03:41  text" per row, oldest first.
    QStringList rows() const;

private:
    QListWidget* list_ = nullptr;
    QDateTime lastTime_;
    QString lastText_;
};

} // namespace native::ui
