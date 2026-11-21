// ui/groups_panel -- see groups_panel.h.
#include "ui/groups_panel.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "core/texplan.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"

namespace native::ui {

namespace tex = native::tools::tex;
namespace tp = core::texplan;
using ojson = nlohmann::ordered_json;

namespace {
constexpr int kThumbW = 96, kThumbH = 72;
constexpr int kPath = Qt::UserRole + 1;
} // namespace

GroupsPanel::GroupsPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("GroupsPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    tabs_ = new QTabBar;
    tabs_->setObjectName(QStringLiteral("tabs"));
    tabs_->addTab(QStringLiteral("Simple"));
    tabs_->setTabToolTip(0, QStringLiteral("Texture sets: wall pieces in rows, 9-slice frames"));
    tabs_->addTab(QStringLiteral("Advanced"));
    tabs_->setTabToolTip(1, QStringLiteral("Rule groups: splits, repeats, conditions, parameters"));
    tabs_->setExpanding(false);
    v->addWidget(tabs_);
    auto* bar = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("search"));
    search_->setPlaceholderText(QStringLiteral("Search texture groups"));
    search_->setClearButtonEnabled(true);
    newAdvanced_ = new QPushButton(QStringLiteral("New advanced group..."));
    newAdvanced_->setObjectName(QStringLiteral("btn:newAdvanced"));
    newAdvanced_->setToolTip(QStringLiteral("Start a rule group from a template"));
    auto* refresh = new QPushButton(QStringLiteral("Rescan"));
    refresh->setObjectName(QStringLiteral("btn:rescan"));
    refresh->setToolTip(QStringLiteral("Rescan the texture group folder"));
    count_ = new QLabel;
    count_->setObjectName(QStringLiteral("count"));
    bar->addWidget(search_, 1);
    bar->addWidget(newAdvanced_);
    bar->addWidget(refresh);
    bar->addWidget(count_);
    v->addLayout(bar);
    empty_ = new QLabel;
    empty_->setObjectName(QStringLiteral("empty"));
    empty_->setWordWrap(true);
    v->addWidget(empty_);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("list"));
    list_->setIconSize(QSize(kThumbW, kThumbH));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->addWidget(list_, 1);

    clickTimer_ = new QTimer(this);
    clickTimer_->setSingleShot(true);
    clickTimer_->setInterval(QApplication::doubleClickInterval());
    connect(clickTimer_, &QTimer::timeout, this, [this] { apply(pending_); });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int i) { setTab(i == 0 ? tex::GroupTab::Simple : tex::GroupTab::Advanced); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { setQuery(t); });
    connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
    connect(newAdvanced_, &QPushButton::clicked, this, [this] { win_.openTexGroupEditor({}); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        pending_ = item->data(kPath).toString().toStdString();
        clickTimer_->start();   // a double click edits instead
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        clickTimer_->stop();
        edit(item->data(kPath).toString().toStdString());
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QListWidgetItem* item = list_->itemAt(p);
        if (!item) return;
        const std::string name = item->data(kPath).toString().toStdString();
        QMenu menu(this);
        menu.addAction(QStringLiteral("Apply to selection"), [this, name] { apply(name); });
        menu.addAction(QStringLiteral("Edit..."), [this, name] { edit(name); });
        menu.addAction(QStringLiteral("Duplicate"), [this, name] { duplicate(name); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete..."), [this, name] {
            const auto answer = QMessageBox::question(this, QStringLiteral("Delete texture group"),
                                                      QString::fromStdString("Delete '" + name + "'? Faces that use it are drawn plainly afterwards."));
            if (answer == QMessageBox::Yes) remove(name);
        });
        menu.exec(list_->viewport()->mapToGlobal(p));
    });
    connect(&win_, &MainWindow::texGroupsChanged, this, [this] { reload(); });
    connect(&win_, &MainWindow::assetsRootChanged, this, [this] { reload(); });
    connect(&win_, &MainWindow::entityDefinitionsChanged, this, [this] { reload(); });
    setTab(tex::GroupTab::Advanced);   // the Simple tab fills with the set editor (T11.4d)
    reload();
}

void GroupsPanel::reload() {
    images_.clear();
    thumbs_.clear();
    entries_.clear();
    for (const std::string& name : win_.texGroupList()) {
        Entry e;
        e.name = name;
        e.title = name;
        if (const auto raw = win_.texGroupRead(name)) {
            e.tab = tex::groupTabOf(*raw);
            if (raw->contains("name") && (*raw)["name"].is_string() && !(*raw)["name"].get<std::string>().empty()) e.title = (*raw)["name"].get<std::string>();
            if (raw->contains("description") && (*raw)["description"].is_string()) e.description = (*raw)["description"].get<std::string>();
        }
        entries_.push_back(std::move(e));
    }
    rebuild();
}

void GroupsPanel::setTab(tex::GroupTab tab) {
    tab_ = tab;
    if (tabs_->currentIndex() != (tab == tex::GroupTab::Simple ? 0 : 1)) {
        tabs_->blockSignals(true);
        tabs_->setCurrentIndex(tab == tex::GroupTab::Simple ? 0 : 1);
        tabs_->blockSignals(false);
    }
    rebuild();
}

void GroupsPanel::setQuery(const QString& query) {
    query_ = query.toStdString();
    if (search_->text() != query) search_->setText(query);
    rebuild();
}

std::vector<std::string> GroupsPanel::shown() const {
    std::vector<std::string> mine;
    for (const Entry& e : entries_)
        if (e.tab == tab_) mine.push_back(e.name);
    return tex::filterGroups(mine, query_);
}

void GroupsPanel::rebuild() {
    clickTimer_->stop();
    list_->clear();
    size_t mine = 0;
    for (const Entry& e : entries_) mine += e.tab == tab_;
    const std::vector<std::string> names = shown();
    for (const std::string& name : names) {
        const Entry* e = nullptr;
        for (const Entry& x : entries_)
            if (x.name == name) e = &x;
        if (!e) continue;
        QString text = QString::fromStdString(e->title == e->name ? e->name : e->title + "\n" + e->name);
        if (!e->description.empty()) text += QStringLiteral("\n") + QString::fromStdString(e->description);
        auto* item = new QListWidgetItem(QIcon(QPixmap::fromImage(thumbnailOf(name))), text, list_);
        item->setData(kPath, QString::fromStdString(name));
        item->setToolTip(QString::fromStdString(name) + QStringLiteral("\nClick: apply to the selection. Double-click: edit. Right-click: more."));
        item->setSizeHint(QSize(100, kThumbH + 8));
    }
    count_->setText(QStringLiteral("%1 of %2").arg(names.size()).arg(mine));
    newAdvanced_->setVisible(tab_ == tex::GroupTab::Advanced);
    empty_->setVisible(mine == 0);
    empty_->setText(tab_ == tex::GroupTab::Simple ? QStringLiteral("No texture sets yet. Sets (wall pieces in rows, 9-slice frames) are made with the set editor.")
                                                   : QStringLiteral("No advanced groups. They are rule sets (splits, repeats, conditions): \"New advanced group\" starts from a template."));
}

QImage GroupsPanel::thumbnailOf(const std::string& name) const {
    const auto it = thumbs_.find(name);
    if (it != thumbs_.end()) return it->second;
    return thumbs_[name] = render(name);
}

QImage GroupsPanel::render(const std::string& name) const {
    QImage img(kThumbW, kThumbH, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x23, 0x23, 0x23));
    const auto raw = win_.texGroupRead(name);
    if (!raw) return img;
    const ojson j = ojson::parse(raw->dump(), nullptr, false);
    if (j.is_discarded()) return img;
    const tp::GroupParse parse = tp::parseGroup(j);
    if (!parse.group) return img;
    tp::LayoutInput in;
    in.group = parse.group.get();
    in.w = 8;
    in.h = 6;
    in.seed = 1;
    in.info = win_.textureInfo();
    tp::LayoutResult r = tp::solveLayout(in);
    GroupPreviewCanvas canvas;
    canvas.setSources([this](const std::string& p) { return win_.textureInfo() ? win_.textureInfo()(p) : nullptr; },
                      [this](const std::string& p) {
                          const auto f = images_.find(p);
                          if (f != images_.end()) return f->second;
                          return images_[p] = win_.loadTextureImage(p);
                      });
    canvas.setMargin(2);
    canvas.setOutlines(false);
    canvas.setLabels(false);
    canvas.resize(kThumbW, kThumbH);
    canvas.setLayout(std::move(r.cells), in.w, in.h);
    canvas.render(&img);
    return img;
}

bool GroupsPanel::apply(const std::string& name) { return win_.applyTexGroup(name); }

void GroupsPanel::edit(const std::string& name) { win_.openTexGroupEditor(name); }

std::string GroupsPanel::duplicate(const std::string& name) {
    const auto raw = win_.texGroupRead(name);
    if (!raw) return {};
    std::vector<std::string> existing;
    for (const Entry& e : entries_) existing.push_back(e.name);
    const std::string copy = tex::copyName(name, existing);
    nlohmann::json value = *raw;
    if (value.contains("name") && value["name"].is_string()) value["name"] = value["name"].get<std::string>() + " (copy)";
    if (!win_.texGroupWrite(copy, value).empty()) return {};
    return copy;
}

std::string GroupsPanel::remove(const std::string& name) { return win_.texGroupRemove(name); }

QWidget* GroupsPanel::part(const QString& name) const { return findChild<QWidget*>(name); }

} // namespace native::ui
