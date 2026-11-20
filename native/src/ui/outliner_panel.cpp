// ui/outliner_panel -- see outliner_panel.h.
#include "ui/outliner_panel.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include "doc/prefabs.h"
#include "doc/selection.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

constexpr int kRowHeight = 22;
constexpr int kIndent = 16;
constexpr int kFoldWidth = 16;
constexpr int kEyeWidth = 20;

// Where the parts of a row are inside its rectangle.
struct RowParts {
    QRect fold, eye;
    int textX = 0;
};

RowParts partsOf(const QRect& r, const tools::OutlinerRow& row) {
    const int x0 = r.left() + 4 + (row.kind == tools::OutlinerRow::Kind::Brush ? kIndent : 0);
    RowParts p;
    p.fold = QRect(x0, r.top(), kFoldWidth, r.height());
    p.eye = QRect(x0 + kFoldWidth, r.top(), kEyeWidth, r.height());
    p.textX = p.eye.right() + 4;
    return p;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

} // namespace

// ---- the model: a list of rows (the view's own selection is not used; rows know whether they are selected) ----------------------------

class OutlinerRowModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows.size()); }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rowCount()) return {};
        const tools::OutlinerRow& r = rows[static_cast<size_t>(index.row())];
        if (role == Qt::DisplayRole) return qs(r.label + " " + r.target + " " + r.detail);
        if (role == Qt::ToolTipRole) return qs(r.key);
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override { return index.isValid() ? Qt::ItemIsEnabled : Qt::NoItemFlags; }
    void setRows(std::vector<tools::OutlinerRow> next) {
        beginResetModel();
        rows = std::move(next);
        endResetModel();
    }
    std::vector<tools::OutlinerRow> rows;
};

namespace {

class RowDelegate : public QStyledItemDelegate {
public:
    RowDelegate(const OutlinerRowModel& m, QObject* parent) : QStyledItemDelegate(parent), model_(m) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {100, kRowHeight}; }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        if (index.row() >= static_cast<int>(model_.rows.size())) return;
        const tools::OutlinerRow& row = model_.rows[static_cast<size_t>(index.row())];
        const QPalette& pal = opt.palette;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        if (row.selected) p->fillRect(opt.rect, QColor(pal.color(QPalette::Highlight)).darker(100));
        else if (opt.state & QStyle::State_MouseOver) p->fillRect(opt.rect, QColor(pal.color(QPalette::Highlight).red(), pal.color(QPalette::Highlight).green(), pal.color(QPalette::Highlight).blue(), 40));
        const RowParts parts = partsOf(opt.rect, row);
        QColor text = row.selected ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
        QColor dim = row.selected ? text : pal.color(QPalette::PlaceholderText);
        if (row.hidden()) {
            text.setAlphaF(0.45f);
            dim.setAlphaF(0.45f);
        }
        // the fold arrow
        if (row.foldable) {
            p->setPen(Qt::NoPen);
            p->setBrush(text);
            const QPointF c = parts.fold.center();
            QPolygonF tri;
            if (row.open) tri << QPointF(c.x() - 4, c.y() - 2) << QPointF(c.x() + 4, c.y() - 2) << QPointF(c.x(), c.y() + 3);
            else tri << QPointF(c.x() - 2, c.y() - 4) << QPointF(c.x() + 3, c.y()) << QPointF(c.x() - 2, c.y() + 4);
            p->drawPolygon(tri);
        }
        // the eye: open = shown, struck through = hidden; dimmer when it cannot be switched here (a rule or the entity hides it)
        {
            QColor eye = row.selected ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
            eye.setAlphaF(tools::eyeLocked(row.eye) ? 0.35f : row.hidden() ? 0.6f : 0.9f);
            const QPointF c = parts.eye.center();
            p->setPen(QPen(eye, 1.3));
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(c, 6.5, 3.8);
            if (row.hidden()) {
                p->drawLine(QPointF(c.x() - 6, c.y() + 5), QPointF(c.x() + 6, c.y() - 5));
            } else {
                p->setBrush(eye);
                p->drawEllipse(c, 1.8, 1.8);
            }
        }
        // the text: class, "target name", id and details
        QFont f = opt.font;
        const QFontMetrics fm(f);
        int x = parts.textX;
        const int baseline = opt.rect.top() + (opt.rect.height() + fm.ascent() - fm.descent()) / 2;
        const auto draw = [&](const QString& s, const QColor& c) {
            p->setPen(c);
            p->drawText(x, baseline, s);
            x += fm.horizontalAdvance(s);
        };
        draw(qs(row.label), text);
        if (!row.target.empty()) draw(QStringLiteral(" “") + qs(row.target) + QStringLiteral("”"), row.selected ? text : QColor(0x6f, 0xa8, 0xdc));
        if (!row.detail.empty()) draw(QStringLiteral("  ") + qs(row.detail), dim);
        p->restore();
    }

private:
    const OutlinerRowModel& model_;
};

} // namespace

// ---- the view ------------------------------------------------------------------------------------------------------------------------

void OutlinerView::mousePressEvent(QMouseEvent* e) {
    const QModelIndex index = indexAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && index.isValid()) {
        const auto* m = static_cast<const OutlinerRowModel*>(model());
        const tools::OutlinerRow& row = m->rows[static_cast<size_t>(index.row())];
        const RowParts parts = partsOf(visualRect(index), row);
        const QPoint pos = e->position().toPoint();
        int part = 0;
        if (row.foldable && parts.fold.contains(pos)) part = 1;
        else if (parts.eye.contains(pos)) part = 2;
        emit rowPressed(index.row(), part, e->modifiers());
        e->accept();
        return;
    }
    QListView::mousePressEvent(e);
}

void OutlinerView::contextMenuEvent(QContextMenuEvent* e) {
    emit contextRequested(e->globalPos());
    e->accept();
}

// ---- the panel -----------------------------------------------------------------------------------------------------------------------

OutlinerPanel::OutlinerPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("outlinerPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("outlinerSearch"));
    search_->setPlaceholderText(QStringLiteral("Search outliner…"));
    search_->setToolTip(QStringLiteral("Filter by class, target name, id, brush or prefab name (Esc clears)"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    layout->addWidget(search_);

    rows_ = new OutlinerRowModel(this);
    view_ = new OutlinerView(this);
    view_->setObjectName(QStringLiteral("outlinerList"));
    view_->setModel(rows_);
    view_->setItemDelegate(new RowDelegate(*rows_, view_));
    view_->setSelectionMode(QAbstractItemView::NoSelection);
    view_->setMouseTracking(true);
    view_->setUniformItemSizes(true);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setFocusPolicy(Qt::NoFocus);   // the keys stay with the viewports
    layout->addWidget(view_, 1);
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setObjectName(QStringLiteral("outlinerMessage"));
    message_->setContentsMargins(6, 2, 6, 2);
    layout->addWidget(message_);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { rebuild(); });
    QObject::connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        state_.query = t.toStdString();
        scheduleRefresh();
    });
    QObject::connect(view_, &OutlinerView::rowPressed, this, [this](int row, int part, Qt::KeyboardModifiers mods) {
        if (row < 0 || row >= static_cast<int>(model_.rows.size())) return;
        const std::string key = model_.rows[static_cast<size_t>(row)].key;
        if (part == 1) clickFold(key);
        else if (part == 2) clickEye(key);
        else clickRow(key, mods);
    });
    QObject::connect(view_, &OutlinerView::contextRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        QAction* unhide = menu.addAction(QStringLiteral("Unhide all"));
        unhide->setEnabled(!win_.hideState().keys.empty());
        QObject::connect(unhide, &QAction::triggered, this, [this] { win_.unhideAll(); });
        QAction* collapse = menu.addAction(QStringLiteral("Collapse all"));
        QObject::connect(collapse, &QAction::triggered, this, [this] {
            state_.expanded.clear();
            scheduleRefresh();
        });
        menu.exec(pos);
    });
    QObject::connect(&win_, &MainWindow::documentEdited, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::documentChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::selectionChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::prefabExpansionChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::hiddenChanged, this, [this] {
        ++hideRevision_;
        scheduleRefresh();
    });
    scheduleRefresh();   // the window is still being built: it fills the list when it is up
}

OutlinerPanel::~OutlinerPanel() = default;

void OutlinerPanel::scheduleRefresh() { timer_->start(); }

void OutlinerPanel::refreshNow() {
    timer_->stop();
    rebuild();
}

void OutlinerPanel::setQuery(const QString& query) { search_->setText(query); }
QString OutlinerPanel::query() const { return search_->text(); }

int OutlinerPanel::rowOfKey(const std::string& key) const {
    for (size_t i = 0; i < model_.rows.size(); ++i)
        if (model_.rows[i].key == key) return static_cast<int>(i);
    return -1;
}

doc::SelRef OutlinerPanel::refOf(const tools::OutlinerRow& row) const {
    switch (row.kind) {
        case tools::OutlinerRow::Kind::Instance: return doc::SelRef::instance(row.instanceId);
        case tools::OutlinerRow::Kind::Brush: {
            // a brush that is the only one of its entity IS the entity
            const auto d = win_.document();
            return d ? doc::wholeEntityIfSole(*d, doc::SelRef::brush(row.entityId, row.brushId)) : doc::SelRef::brush(row.entityId, row.brushId);
        }
        case tools::OutlinerRow::Kind::Entity: break;
    }
    return doc::SelRef::entity(row.entityId);
}

void OutlinerPanel::clickRow(const std::string& key, Qt::KeyboardModifiers mods) {
    const int i = rowOfKey(key);
    if (i < 0) return;
    const tools::OutlinerRow& row = model_.rows[static_cast<size_t>(i)];
    win_.selectFromOutliner(refOf(row), row.hidden(), mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::MetaModifier));
}

void OutlinerPanel::clickFold(const std::string& key) {
    const int i = rowOfKey(key);
    if (i < 0 || !model_.rows[static_cast<size_t>(i)].foldable) return;
    const std::string& id = model_.rows[static_cast<size_t>(i)].entityId;
    if (!state_.expanded.erase(id)) state_.expanded.insert(id);
    refreshNow();
}

void OutlinerPanel::clickEye(const std::string& key) {
    const int i = rowOfKey(key);
    if (i < 0) return;
    if (tools::eyeLocked(model_.rows[static_cast<size_t>(i)].eye)) return;   // hidden through a rule or its entity: switched there
    win_.toggleHiddenKey(key);
    refreshNow();
}

bool OutlinerPanel::eventFilter(QObject* watched, QEvent* e) {
    if (watched == search_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape && !search_->text().isEmpty()) {
            search_->clear();   // typing must not trigger viewport shortcuts: the key stops here
            return true;
        }
    }
    return QWidget::eventFilter(watched, e);
}

void OutlinerPanel::rebuild() {
    const std::shared_ptr<const doc::DocumentVersion> d = win_.document();
    const std::vector<doc::SelRef>& sel = win_.selection().items();
    // Rebuilding on every change would eat pointer events; only rebuild when the tree, the selection or the hide state changed.
    std::string sig = std::to_string(d ? d->version : 0) + "|" + std::to_string(win_.selection().revision()) + "|" + std::to_string(hideRevision_) + "|" + state_.query + "|";
    for (const std::string& e : state_.expanded) sig += e + ",";
    if (const auto exp = win_.prefabExpansion()) sig += "|" + std::to_string(exp->documentVersion) + "/" + std::to_string(exp->problems().size());
    if (sig == signature_ && rebuilds_ > 0) return;
    signature_ = sig;
    ++rebuilds_;

    model_ = d ? tools::buildOutliner(*d, sel, win_.hideState(), state_) : tools::OutlinerModel{};
    if (!d) model_.message = "No map open.";
    const int scroll = view_->verticalScrollBar()->value();   // a rebuild (an eye clicked, a fold) must not move the list
    rows_->setRows(model_.rows);
    view_->verticalScrollBar()->setValue(scroll);
    std::string text = model_.message;
    if (const auto exp = win_.prefabExpansion())
        for (const std::string& p : exp->problems()) text += (text.empty() ? "" : "\n") + std::string("\xE2\x9A\xA0 ") + p;
    message_->setText(qs(text));
    message_->setVisible(!text.empty());

    // scroll to the primary selection when it changed (nothing moves for a row that is fully visible already)
    const std::vector<std::string> keys = reveal_.update(win_.selection().primary());
    for (const std::string& k : keys) {
        const int i = rowOfKey(k);
        if (i < 0) continue;
        const QModelIndex index = rows_->index(i);
        const QRect vr = view_->visualRect(index);
        if (!view_->viewport()->rect().contains(vr)) view_->scrollTo(index, QAbstractItemView::PositionAtCenter);
        break;
    }
}

} // namespace native::ui
