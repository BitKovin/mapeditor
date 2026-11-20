// ui/details_panel -- the Details dock: the form of the selected entity, brush or prefab instance (rows from tools/details, built from the .ent definitions: classname,
// id, targetname, origin, brush operation, spawnflags, angles, scale, typed properties with the widget of each key's type, other keyvalues, reference pickers; for a
// prefab instance its name, transform, contents with the override editor and the override list). The panel draws the rows and reports every edit to the main window
// (one undo step each); tools/details decides what an edit does.
// While a field of the form has the keyboard the form is not rebuilt (typing must not lose the caret); it catches up when the focus leaves.
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.4, 10_UI_AND_INPUT.md.
#pragma once

#include <QWidget>

#include <functional>
#include <map>
#include <set>
#include <string>

#include "tools/details.h"

class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace native::ui {

class MainWindow;

class DetailsPanel : public QWidget {
    Q_OBJECT
public:
    explicit DetailsPanel(MainWindow& window, QWidget* parent = nullptr);
    ~DetailsPanel() override;

    // The form shown now (after the last rebuild; refreshNow() catches up with the window).
    const tools::DetailsForm& form() const { return form_; }
    void refreshNow();
    // Makes an edit as if the user had changed a row (the widgets call this; tests too). False when nothing was committed.
    bool apply(const tools::DetailsEdit& edit, const tools::EditValue& value);
    // The widget that edits the row `label` (objectName "row:<label>"); null when there is none. Spin boxes of a vector row are "row:<label>:0..2",
    // the pickers of a reference "row:<label>:ref0..", "row:<label>:add".
    QWidget* editor(const QString& label) const;
    int rebuilds() const { return rebuilds_; }
    // The entity of the selected instance whose override editor is open (empty = none).
    void openOverride(const std::string& path);

private:
    void scheduleRefresh(bool force = false);
    void rebuild();
    void build();
    bool focusInside() const;
    std::string selectionKey() const;

    // row builders
    QWidget* rowWidget(const tools::DetailsRow& row, QWidget* editor, QWidget* trailing = nullptr);
    QWidget* makeRow(const tools::DetailsRow& row);
    QWidget* numberEdit(const tools::DetailsRow& row, const std::string& text, const std::function<void(double)>& commit, const QString& name);
    QWidget* vecEdit(const tools::DetailsRow& row);
    QWidget* refEdit(const tools::DetailsRow& row);
    QWidget* addKeyRow(const tools::DetailsRow& row);

    MainWindow& win_;
    QScrollArea* scroll_ = nullptr;
    QTimer* timer_ = nullptr;
    tools::DetailsForm form_;
    std::string signature_;
    std::string shownSelection_;
    bool stale_ = false;
    bool force_ = false;
    int rebuilds_ = 0;
    std::string openPath_;
    std::map<std::string, std::set<std::string>> pendingRefs_;   // entity id -> reference keys added with "Add" that have no target yet
};

} // namespace native::ui
