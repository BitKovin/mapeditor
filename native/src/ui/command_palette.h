// ui/command_palette -- the command palette (F3 / Ctrl+Shift+P): a search box over the list of every action that can run now, with its keys and category.
// The model is app/actions/palette (fuzzy matching, typed arguments, the recently used list); this is only the dialog around it. Up / Down move in the list while
// the search box keeps the keyboard, Enter runs the chosen entry, Esc closes. The dialog closes first, then the action runs (so a dialog it opens is not nested).
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md §6, 08_MILESTONES.md T4.7.
#pragma once

#include <QDialog>

#include <optional>
#include <vector>

#include "app/actions/palette.h"

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace native::ui {

class MainWindow;

class CommandPalette : public QDialog {
    Q_OBJECT
public:
    // Lists the actions that are enabled for the window's current state, the recently used ones first.
    explicit CommandPalette(MainWindow& window);

    // The search text and the rows it gives (best first). Setting the text refreshes the list and selects the first row.
    void setQuery(const QString& text);
    QString query() const;
    int rowCount() const;
    const actions::PaletteEntry& row(int index) const { return rows_[static_cast<size_t>(index)]; }
    int currentRow() const;
    void setCurrentRow(int index);

    // Closes the dialog with the entry of `index` (the current one for -1) as the choice; false for an empty list.
    bool choose(int index = -1);
    const std::optional<actions::PaletteEntry>& chosen() const { return chosen_; }
    // Runs the choice through the window (and remembers it as recently used). False when nothing was chosen or the action refused.
    bool runChosen();

    QLineEdit* searchBox() const { return search_; }
    QTreeWidget* list() const { return list_; }

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void refresh();

    MainWindow& win_;
    std::vector<actions::PaletteEntry> entries_;
    std::vector<actions::PaletteEntry> rows_;
    std::vector<actions::Recent> recent_;
    std::optional<actions::PaletteEntry> chosen_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QLabel* detail_ = nullptr;
};

} // namespace native::ui
