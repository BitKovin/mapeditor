// ui/outliner_panel -- the Outliner dock: a search box over the entities (brushes folded under them) and prefab instances of the document, with an eye on every row
// (hide / show), the fold arrow of entities with brushes, a highlight for the selection and scrolling to the primary selection when it changes. The rows come from
// tools/outliner (Qt-free, tested); this widget draws them and sends clicks to the main window.
// Thread: UI thread only.
// Spec: 08_MILESTONES.md T4.4, 10_UI_AND_INPUT.md.
#pragma once

#include <QListView>
#include <QWidget>

#include <string>
#include <vector>

#include "tools/outliner.h"

class QLabel;
class QLineEdit;
class QTimer;

namespace native::ui {

class MainWindow;
class OutlinerRowModel;

class OutlinerView : public QListView {
    Q_OBJECT
public:
    using QListView::QListView;
signals:
    void rowPressed(int row, int part, Qt::KeyboardModifiers mods);   // part: 0 the row, 1 its fold arrow, 2 its eye
    void contextRequested(const QPoint& globalPos);
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
};

class OutlinerPanel : public QWidget {
    Q_OBJECT
public:
    explicit OutlinerPanel(MainWindow& window, QWidget* parent = nullptr);
    ~OutlinerPanel() override;

    // The rows shown now (after the last rebuild; call refreshNow() to catch up with the window).
    const tools::OutlinerModel& model() const { return model_; }
    void refreshNow();
    void setQuery(const QString& query);
    QString query() const;
    QLineEdit* searchBox() const { return search_; }
    OutlinerView* view() const { return view_; }

    // What a click on a row does (the view calls these; tests too).
    void clickRow(const std::string& key, Qt::KeyboardModifiers mods = {});
    void clickFold(const std::string& key);
    void clickEye(const std::string& key);

    // The index of a row in model().rows, -1 when it is not listed.
    int rowOfKey(const std::string& key) const;
    // How often the list was rebuilt (tests: an unrelated change must not rebuild it).
    int rebuilds() const { return rebuilds_; }

private:
    void scheduleRefresh();
    void rebuild();
    bool eventFilter(QObject* watched, QEvent* e) override;
    doc::SelRef refOf(const tools::OutlinerRow& row) const;

    MainWindow& win_;
    QLineEdit* search_ = nullptr;
    OutlinerView* view_ = nullptr;
    OutlinerRowModel* rows_ = nullptr;
    QLabel* message_ = nullptr;
    QTimer* timer_ = nullptr;
    tools::OutlinerModel model_;
    tools::OutlinerState state_;
    tools::RevealTracker reveal_;
    std::string signature_;
    int rebuilds_ = 0;
    uint64_t hideRevision_ = 0;
};

} // namespace native::ui
