// ui/messages_panel -- see messages_panel.h.
#include "ui/messages_panel.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include "ui/main_window.h"

namespace native::ui {

MessagesPanel::MessagesPanel(MainWindow& window, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MessagesPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("messagesList"));
    list_->setToolTip(QStringLiteral("What the status bar said, with the time."));
    list_->setWordWrap(true);
    v->addWidget(list_, 1);
    auto* bar = new QHBoxLayout;
    auto* clear = new QPushButton(QStringLiteral("Clear"), this);
    clear->setObjectName(QStringLiteral("messagesClear"));
    QObject::connect(clear, &QPushButton::clicked, this, [this] { this->clear(); });
    bar->addStretch(1);
    bar->addWidget(clear);
    v->addLayout(bar);
    QObject::connect(window.statusBar(), &QStatusBar::messageChanged, this, [this](const QString& text) { add(text); });
}

void MessagesPanel::add(const QString& text, const QDateTime& when) {
    if (text.isEmpty() || text == lastText_) return;
    const QString row = when.toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text;
    // a reading that follows the mouse: soon after the previous message and starting like it
    int common = 0;
    while (common < text.size() && common < lastText_.size() && text[common] == lastText_[common]) ++common;
    const bool alike = common >= std::min(8, static_cast<int>(std::min(text.size(), lastText_.size())));
    const bool merge = alike && list_->count() > 0 && lastTime_.isValid() && lastTime_.msecsTo(when) >= 0 && lastTime_.msecsTo(when) < kMergeMs;
    if (merge) {
        list_->item(list_->count() - 1)->setText(row);
    } else {
        list_->addItem(row);
        while (list_->count() > kMaxMessages) delete list_->takeItem(0);
    }
    lastTime_ = when;
    lastText_ = text;
    list_->scrollToBottom();
}

void MessagesPanel::clear() {
    list_->clear();
    lastText_.clear();
    lastTime_ = QDateTime();
}

QStringList MessagesPanel::rows() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->text();
    return out;
}

} // namespace native::ui
