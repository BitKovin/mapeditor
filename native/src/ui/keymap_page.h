// ui/keymap_page -- Settings > Keymap, the binding editor: a searchable table of every action with its bindings, filters (changed / unbound / conflicts, category, context),
// the recorder for a new binding (BindingRecorder: record the input, pick the context, see what it clashes with), removing and resetting, the reverse lookup ("press a key to see
// what it does"), undo / redo, and the user keymaps (switch, new, delete, import, export). Every change applies at once: the router reads the same keymap, the menus show the new
// keys, the user keymap file is written. The decisions live in app/actions/keymap_edit; this is the widget side.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md §5, 08_MILESTONES.md T4.6.
#pragma once

#include <QDialog>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/actions/keymap_edit.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;

namespace native::ui {

class ChordCapture;
class MainWindow;

// The dialog behind "+": records the input of a new binding of one action.
class BindingRecorder : public QDialog {
    Q_OBJECT
public:
    BindingRecorder(MainWindow& window, const actions::ActionDef& def, QWidget* parent = nullptr);
    ChordCapture* capture() const { return capture_; }
    // The binding as entered (empty `error` when it is a valid one); the conflicts it would have are shown in the dialog.
    struct Entry {
        actions::Binding binding;
        std::string error;
    };
    Entry entry() const;
    void setContext(const std::string& context);
    void setInput(const QString& text);

private:
    void refresh();

    MainWindow& win_;
    const actions::ActionDef& def_;
    ChordCapture* capture_ = nullptr;
    QComboBox *mode_ = nullptr, *context_ = nullptr;
    QCheckBox *physical_ = nullptr, *typeInstead_ = nullptr;
    QLineEdit* params_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* ok_ = nullptr;
};

class KeymapPage : public QWidget {
    Q_OBJECT
public:
    explicit KeymapPage(MainWindow& window, QWidget* parent = nullptr);

    actions::BindingEditor& editor() { return *editor_; }
    // Rebuilds the table, the binding list and the keymap controls from the keymap (done after every change).
    void refresh();
    // The actions in the table, in order (after the filters).
    std::vector<std::string> shownActions() const;
    void selectAction(const std::string& id);
    std::string selectedAction() const;

    // What the "+" button does after the recorder: add with the conflict question. `onConflict` Cancel = ask through the handler.
    actions::BindingEditor::Result addBinding(const actions::Binding& binding);
    bool removeBinding(const actions::Binding& binding);
    // What the dialogs behind the buttons decide; each returns "" or why it failed.
    std::string createKeymap(const std::string& name);
    std::string removeKeymap(const std::string& name);
    std::string importKeymap(const std::string& file, const std::string& name);
    std::string exportKeymap(const std::string& file);

    // The question "Already used by ...": Replace, Keep both or Cancel. The default is a message box; tests install an answer.
    using ConflictHandler = std::function<actions::BindingEditor::Resolve(const std::vector<actions::Conflict>&)>;
    void setConflictHandler(ConflictHandler h) { conflictHandler_ = std::move(h); }
    // The reverse lookup for a typed or recorded chord.
    void lookup(const QString& chord);

private:
    void edited();
    void filterChanged();
    void fillBindings();
    void addForSelected();
    void removeSelectedBinding();
    actions::RowFilter filter() const;

    MainWindow& win_;
    std::unique_ptr<actions::BindingEditor> editor_;
    ConflictHandler conflictHandler_;
    QLineEdit* search_ = nullptr;
    QComboBox *show_ = nullptr, *category_ = nullptr, *context_ = nullptr, *keymaps_ = nullptr;
    QTreeWidget* table_ = nullptr;
    QListWidget* bindings_ = nullptr;
    QLabel *description_ = nullptr, *lookupResult_ = nullptr, *note_ = nullptr;
    ChordCapture* lookupCapture_ = nullptr;
    QPushButton *add_ = nullptr, *remove_ = nullptr, *resetAction_ = nullptr, *resetAll_ = nullptr, *undo_ = nullptr, *redo_ = nullptr;
    QPushButton *new_ = nullptr, *delete_ = nullptr, *import_ = nullptr, *export_ = nullptr;
    std::vector<actions::Binding> listed_;   // the bindings in `bindings_`
    bool refreshing_ = false;
    bool ownChange_ = false;   // the keymap changed because of this page (no need to start a new edit history)
};

} // namespace native::ui
