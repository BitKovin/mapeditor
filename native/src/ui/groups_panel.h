// ui/groups_panel -- the Groups dock (shell/src/ui/texGroupsPanel.ts): the texture groups of the game, in two tabs (Simple: texture sets, Advanced: rule groups), each with a thumbnail of
// the group laid out on a sample wall (drawn by the same solver as the viewports). A click applies the group to the selected faces / brushes, a double click edits it, the context
// menu duplicates and deletes. The list comes from MainWindow::texGroupList (Core's group folder) and is read again after every write and when the game profile changes.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md B (texGroupsPanel), 08_MILESTONES.md M11 item 4.
#pragma once

#include <QImage>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "tools/texturing/group_tools.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabBar;
class QTimer;

namespace native::ui {

class MainWindow;

class GroupsPanel : public QWidget {
    Q_OBJECT
public:
    explicit GroupsPanel(MainWindow& window, QWidget* parent = nullptr);

    // Reads the list and every group again.
    void reload();
    struct Entry {
        std::string name, title, description;
        native::tools::tex::GroupTab tab = native::tools::tex::GroupTab::Advanced;
    };
    const std::vector<Entry>& entries() const { return entries_; }
    native::tools::tex::GroupTab tab() const { return tab_; }
    void setTab(native::tools::tex::GroupTab tab);
    void setQuery(const QString& query);
    std::vector<std::string> shown() const;   // the names in the list
    // The actions of the list (a click, a double click, the context menu); tests call them directly.
    bool apply(const std::string& name);
    void edit(const std::string& name);
    std::string duplicate(const std::string& name);   // the new name, "" on failure
    std::string remove(const std::string& name);      // "" = removed (the confirmation is the menu's)
    QWidget* part(const QString& name) const;
    QImage thumbnailOf(const std::string& name) const;

private:
    void rebuild();
    QImage render(const std::string& name) const;

    MainWindow& win_;
    std::vector<Entry> entries_;
    native::tools::tex::GroupTab tab_ = native::tools::tex::GroupTab::Simple;
    std::string query_;
    QTabBar* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* empty_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* newAdvanced_ = nullptr;
    QTimer* clickTimer_ = nullptr;
    std::string pending_;
    mutable std::map<std::string, QImage> images_;   // decoded textures, until the next reload
    mutable std::map<std::string, QImage> thumbs_;
};

} // namespace native::ui
