// ui/adjust_panel -- see adjust_panel.h.
#include "ui/adjust_panel.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/main_window.h"

namespace native::ui {

AdjustPanel::AdjustPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("AdjustPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("adjustTitle"));
    QFont f = title_->font();
    f.setBold(true);
    title_->setFont(f);
    v->addWidget(title_);
    auto* box = new QWidget(this);
    fields_ = new QVBoxLayout(box);
    fields_->setContentsMargins(0, 0, 0, 0);
    v->addWidget(box);
    hint_ = new QLabel(QStringLiteral("Nothing to adjust. After an operation with a number (Brush > Hollow) its settings show here until you do something else."), this);
    hint_->setObjectName(QStringLiteral("adjustHint"));
    hint_->setWordWrap(true);
    hint_->setEnabled(false);
    v->addWidget(hint_);
    v->addStretch(1);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    applyTimer_ = new QTimer(this);
    applyTimer_->setSingleShot(true);
    applyTimer_->setInterval(250);
    QObject::connect(applyTimer_, &QTimer::timeout, this, [this] { applyNow(); });
    const auto later = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::lastOperationChanged, this, later);
    QObject::connect(&win_, &MainWindow::documentEdited, this, later);
    QObject::connect(&win_, &MainWindow::documentChanged, this, later);
    refreshNow();
}

QString AdjustPanel::title() const { return title_->text(); }

QDoubleSpinBox* AdjustPanel::field(const QString& key) const {
    for (QDoubleSpinBox* s : spins_)
        if (s->objectName() == QStringLiteral("adjust:") + key) return s;
    return nullptr;
}

void AdjustPanel::refreshNow() {
    timer_->stop();
    const LastOperation* op = win_.lastOperation();
    // while the user is typing in a field the form is not rebuilt under them, unless it is another operation
    const bool same = op && !spins_.empty() && title_->text() == QString::fromStdString(op->label) && static_cast<size_t>(spins_.size()) == op->params.size();
    if (same && applyTimer_->isActive()) return;
    filling_ = true;
    applyTimer_->stop();
    spins_.clear();
    while (QLayoutItem* it = fields_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    title_->setText(op ? QString::fromStdString(op->label) : QString());
    title_->setVisible(op != nullptr);
    hint_->setVisible(op == nullptr);
    if (op) {
        for (const AdjustParam& p : op->params) {
            auto* row = new QWidget;
            auto* h = new QFormLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* s = new QDoubleSpinBox;
            s->setObjectName(QStringLiteral("adjust:") + QString::fromStdString(p.key));
            s->setDecimals(3);
            s->setRange(p.min, p.max);
            s->setSingleStep(p.step);
            s->setValue(p.value);
            QObject::connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
                if (!filling_) applyTimer_->start();
            });
            h->addRow(QString::fromStdString(p.label), s);
            fields_->addWidget(row);
            spins_.push_back(s);
        }
    }
    filling_ = false;
}

bool AdjustPanel::applyNow() {
    applyTimer_->stop();
    if (spins_.empty() || !win_.lastOperation()) return false;
    std::vector<double> values;
    for (const QDoubleSpinBox* s : spins_) values.push_back(s->value());
    const bool ok = win_.adjustLastOperation(values);
    refreshNow();
    return ok;
}

} // namespace native::ui
