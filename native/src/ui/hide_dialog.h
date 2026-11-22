// ui/hide_dialog -- the Hide popover of the Shell (shell/src/ui/hideMenu.ts): the hide rules with an on / off box each (a rule the user added can be removed), a field to add a class
// pattern (func_*, trigger_*), the brush entity classes found in the map ("Volumes in this map") with a box each, and "Show all classes". A view setting: nothing here is part of the
// map or an undo step; hidden entities stay in the map and in the export. The dialog stays open (it is not modal) and follows the rules, the map and its classes.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md (hideMenu), 08_MILESTONES.md T11.10e.
#pragma once

#include <QDialog>

#include <string>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace native::ui {

class MainWindow;

class HideDialog : public QDialog {
    Q_OBJECT
public:
    explicit HideDialog(MainWindow& window, QWidget* parent = nullptr);

    /// Fills the lists from the window's hide rules and the classes of the current document.
    void refresh();
    /// A named child widget: "rule:<index>" (check box), "remove:<index>", "input", "add", "class:<classname>" (check box), "showAll", "empty", "summary"; null when there is none.
    QWidget* part(const QString& name) const;

    // The edits of the widgets (tests call these too).
    void setRuleEnabled(int index, bool on);
    void removeRule(int index);
    bool addPattern(const QString& text);              // false for text that is no pattern
    void setClassHidden(const std::string& classname, bool hidden);
    void showAllClasses();                             // every rule off (hidden brushes and entities from the outliner stay hidden)

private:
    MainWindow& win_;
    QLabel* summary_ = nullptr;
    QVBoxLayout *rules_ = nullptr, *classes_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* showAll_ = nullptr;
    bool filling_ = false;
};

} // namespace native::ui
