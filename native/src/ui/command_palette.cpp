// ui/command_palette -- see command_palette.h.
#include "ui/command_palette.h"

#include <algorithm>

#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/main_window.h"

namespace native::ui {

namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }
constexpr int kMaxRows = 300;
} // namespace

CommandPalette::CommandPalette(MainWindow& window) : QDialog(&window), win_(window) {
    setObjectName(QStringLiteral("palette"));
    setWindowTitle(QStringLiteral("Command palette"));
    resize(640, 420);
    auto* layout = new QVBoxLayout(this);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("palette:search"));
    search_->setPlaceholderText(QStringLiteral("Type a command (grid 0.25 sets a grid size)"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("palette:list"));
    list_->setColumnCount(3);
    list_->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Keys"), QStringLiteral("Category")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setFocusPolicy(Qt::NoFocus);   // the search box keeps the keyboard
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(list_, 1);
    detail_ = new QLabel(this);
    detail_->setObjectName(QStringLiteral("palette:detail"));
    detail_->setWordWrap(true);
    detail_->setMinimumHeight(detail_->fontMetrics().height() * 2 + 6);
    detail_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(detail_);

    UiEnv ui = win_.uiEnv();
    actions::ActionEnv env{&ui};
    entries_ = actions::paletteEntries(win_.registry(), win_.keymap(), env);
    recent_ = win_.paletteRecent();
    search_->installEventFilter(this);
    QObject::connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    QObject::connect(search_, &QLineEdit::returnPressed, this, [this] { choose(); });
    QObject::connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem*, int) { choose(); });
    QObject::connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        const int i = item ? list_->indexOfTopLevelItem(item) : -1;
        detail_->setText(i >= 0 && i < static_cast<int>(rows_.size()) ? qs(rows_[static_cast<size_t>(i)].description) : QString());
    });
    refresh();
    search_->setFocus();
}

void CommandPalette::refresh() {
    rows_ = actions::paletteSearch(entries_, win_.registry(), search_->text().toStdString(), recent_);
    if (rows_.size() > kMaxRows) rows_.resize(kMaxRows);
    list_->setUpdatesEnabled(false);
    list_->clear();
    for (const actions::PaletteEntry& e : rows_) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(0, qs(e.label));
        item->setText(1, qs(e.keys));
        item->setText(2, qs(e.category));
        if (!e.description.empty()) item->setToolTip(0, qs(e.description));
    }
    if (list_->topLevelItemCount() > 0) list_->setCurrentItem(list_->topLevelItem(0));
    else detail_->setText(search_->text().isEmpty() ? QString() : QStringLiteral("No command matches."));
    list_->setUpdatesEnabled(true);
}

void CommandPalette::setQuery(const QString& text) { search_->setText(text); }
QString CommandPalette::query() const { return search_->text(); }
int CommandPalette::rowCount() const { return static_cast<int>(rows_.size()); }

int CommandPalette::currentRow() const {
    QTreeWidgetItem* item = list_->currentItem();
    return item ? list_->indexOfTopLevelItem(item) : -1;
}

void CommandPalette::setCurrentRow(int index) {
    if (index >= 0 && index < list_->topLevelItemCount()) list_->setCurrentItem(list_->topLevelItem(index));
}

bool CommandPalette::choose(int index) {
    if (index < 0) index = currentRow();
    if (index < 0 || index >= static_cast<int>(rows_.size())) return false;
    chosen_ = rows_[static_cast<size_t>(index)];
    accept();
    return true;
}

bool CommandPalette::runChosen() {
    if (!chosen_) return false;
    const actions::PaletteEntry e = *chosen_;
    chosen_.reset();
    const bool ran = win_.runAction(e.id, e.params);
    if (ran) win_.setPaletteRecent(actions::recentUsed(recent_, {e.id, e.params}));
    return ran;
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* e) {
    if (watched == search_ && e->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(e);
        int step = 0;
        switch (k->key()) {
            case Qt::Key_Down: step = 1; break;
            case Qt::Key_Up: step = -1; break;
            case Qt::Key_PageDown: step = 8; break;
            case Qt::Key_PageUp: step = -8; break;
            default: break;
        }
        if (step != 0) {
            const int n = list_->topLevelItemCount();
            if (n > 0) setCurrentRow(std::clamp(currentRow() + step, 0, n - 1));
            return true;
        }
    }
    return QDialog::eventFilter(watched, e);
}

} // namespace native::ui
