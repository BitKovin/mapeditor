// ui/status_bar -- the permanent fields on the right of the status bar (the left part stays the transient message line): the tool, the grid and snap of the
// active viewport, what is selected, the map (with * while it has unsaved changes) and the game folder (a click opens Settings). The texts come from tools/status_text.
// Refreshes are coalesced: any number of changes in one event-loop turn rebuild the labels once (and only when a text really differs).
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.5.
#pragma once

#include <QObject>
#include <QString>

class QLabel;
class QTimer;
class QToolButton;

namespace native::ui {

class MainWindow;

class StatusFields : public QObject {
    Q_OBJECT
public:
    explicit StatusFields(MainWindow& window);

    // The texts as shown now (after refreshNow).
    QString toolText() const;
    QString gridText() const;
    QString selectionText() const;
    QString mapText() const;
    QString folderText() const;
    // The keys of the active tool / modal operator ("LMB / Enter confirm  ·  RMB / Escape cancel"), made from the keymap.
    QString hintText() const;
    // Recomputes the labels at once (tests; the window schedules it).
    void refreshNow();
    // How many times the label texts really changed (a cheap way for tests to see that nothing was rebuilt without a reason).
    int updates() const { return updates_; }
    QToolButton* folderButton() const { return folder_; }

private:
    void schedule();
    void set(QLabel* label, const QString& text);

    MainWindow& win_;
    QLabel *hint_ = nullptr, *tool_ = nullptr, *grid_ = nullptr, *selection_ = nullptr, *map_ = nullptr;
    QToolButton* folder_ = nullptr;
    QTimer* timer_ = nullptr;
    int updates_ = 0;
};

} // namespace native::ui
