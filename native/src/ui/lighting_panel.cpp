// ui/lighting_panel -- see lighting_panel.h.
#include "ui/lighting_panel.h"

#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "doc/selection.h"
#include "tools/light_list.h"
#include "tools/status_text.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

constexpr int kIdRole = Qt::UserRole;
enum Column { Name, Class, Brightness, Mute, Solo, ColumnCount };

QString qs(const std::string& s) { return QString::fromStdString(s); }

QIcon swatch(const QString& color) {
    QPixmap p(12, 12);
    p.fill(QColor(color));
    return QIcon(p);
}

} // namespace

LightingPanel::LightingPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("LightingPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("lightsSummary"));
    summary_->setWordWrap(true);
    v->addWidget(summary_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("lightsTree"));
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setColumnCount(ColumnCount);
    tree_->setHeaderLabels({QStringLiteral("Light"), QStringLiteral("Class"), QStringLiteral("Brightness"), QStringLiteral("Mute"), QStringLiteral("Solo")});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    for (const int c : {Class, Brightness, Mute, Solo}) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    tree_->setToolTip(QStringLiteral("Mute: the light gives no light in the live lighting. Solo: only this light does. Editor only: the map and the export are not changed."));
    v->addWidget(tree_, 1);
    auto* bar = new QHBoxLayout;
    clear_ = new QPushButton(QStringLiteral("All lights on"), this);
    clear_->setObjectName(QStringLiteral("lightsClear"));
    bar->addStretch(1);
    bar->addWidget(clear_);
    v->addLayout(bar);

    QObject::connect(clear_, &QPushButton::clicked, this, [this] { win_.clearLightFilter(); });
    QObject::connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (filling_ || (column != Mute && column != Solo)) return;
        const std::string id = item->data(Name, kIdRole).toString().toStdString();
        const bool on = item->checkState(column) == Qt::Checked;
        if (column == Mute) win_.setLightMuted(id, on);
        else win_.setLightSolo(on ? id : std::string());
    });
    QObject::connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == Mute || column == Solo) return;
        const std::string id = item->data(Name, kIdRole).toString().toStdString();
        if (win_.document() && win_.document()->findEntity(id) >= 0) win_.selection().select(doc::SelRef::entity(id));
    });
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(60);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    const auto later = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::documentEdited, this, later);
    QObject::connect(&win_, &MainWindow::documentChanged, this, later);
    QObject::connect(&win_, &MainWindow::lightFilterChanged, this, later);
    later();   // the window is still being built: the list is filled when it is up
}

void LightingPanel::refreshNow() {
    timer_->stop();
    filling_ = true;
    tree_->clear();
    int lights = 0, muted = 0;
    QString soloName;
    if (const auto d = win_.document()) {
        for (const tools::LightRow& r : tools::listLights(*d)) {
            ++lights;
            auto* item = new QTreeWidgetItem(tree_);
            item->setText(Name, qs(r.name));
            item->setData(Name, kIdRole, qs(r.id));
            item->setIcon(Name, swatch(qs(r.color)));
            item->setText(Class, qs(r.classname));
            item->setText(Brightness, qs(tools::shortNumber(r.brightness)));
            item->setTextAlignment(Brightness, Qt::AlignRight | Qt::AlignVCenter);
            const bool isMuted = win_.lightMuted(r.id), isSolo = win_.lightSolo() == r.id;
            item->setCheckState(Mute, isMuted ? Qt::Checked : Qt::Unchecked);
            item->setCheckState(Solo, isSolo ? Qt::Checked : Qt::Unchecked);
            if (isMuted) ++muted;
            if (isSolo) soloName = qs(r.name);
        }
    }
    QString text = lights == 1 ? QStringLiteral("1 light") : QStringLiteral("%1 lights").arg(lights);
    if (muted) text += QStringLiteral(", %1 muted").arg(muted);
    if (!win_.lightSolo().empty()) text += QStringLiteral(", solo: ") + (soloName.isEmpty() ? qs(win_.lightSolo()) : soloName);
    if (win_.lightFilter()) text += QStringLiteral(" (editor only: the map and the export are not changed)");
    summary_->setText(text);
    clear_->setEnabled(win_.lightFilter() != nullptr);
    filling_ = false;
}

QStringList LightingPanel::rows() const {
    QStringList out;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* it = tree_->topLevelItem(i);
        out << it->text(Name) + QStringLiteral(" | ") + it->text(Class) + QStringLiteral(" | ") + it->text(Brightness) + QStringLiteral(" | ") +
                   (it->checkState(Mute) == Qt::Checked ? QStringLiteral("x") : QString()) + QStringLiteral(" | ") + (it->checkState(Solo) == Qt::Checked ? QStringLiteral("x") : QString());
    }
    return out;
}

QString LightingPanel::summary() const { return summary_->text(); }

void LightingPanel::setMuteBox(const QString& id, bool checked) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (QTreeWidgetItem* it = tree_->topLevelItem(i); it->data(Name, kIdRole).toString() == id) it->setCheckState(Mute, checked ? Qt::Checked : Qt::Unchecked);
}

void LightingPanel::setSoloBox(const QString& id, bool checked) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (QTreeWidgetItem* it = tree_->topLevelItem(i); it->data(Name, kIdRole).toString() == id) it->setCheckState(Solo, checked ? Qt::Checked : Qt::Unchecked);
}

} // namespace native::ui
