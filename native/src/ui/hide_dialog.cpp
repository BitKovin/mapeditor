// ui/hide_dialog -- see hide_dialog.h.
#include "ui/hide_dialog.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "tools/visibility.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }

void clearLayout(QLayout* l) {
    while (QLayoutItem* item = l->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->setParent(nullptr);   // not found by part() any more, deleted when the event loop runs
            w->deleteLater();
        }
        if (QLayout* inner = item->layout()) clearLayout(inner);
        delete item;
    }
}

} // namespace

HideDialog::HideDialog(MainWindow& window, QWidget* parent) : QDialog(parent ? parent : &window), win_(window) {
    setObjectName(QStringLiteral("hideDialog"));
    setWindowTitle(QStringLiteral("Hide"));
    setWindowFlag(Qt::Tool, true);
    resize(340, 420);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral("Hidden things are not drawn or picked but stay in the map and the export. Nothing here is saved in the map."), this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("hide:summary"));
    v->addWidget(summary_);

    v->addWidget(new QLabel(QStringLiteral("<b>Hide by class</b>"), this));
    rules_ = new QVBoxLayout;
    v->addLayout(rules_);
    auto* addRow = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("hide:input"));
    input_->setPlaceholderText(QStringLiteral("class or pattern, e.g. func_*"));
    input_->setToolTip(QStringLiteral("A class name; * matches anything (trigger_* = every class starting with trigger_)"));
    auto* add = new QPushButton(QStringLiteral("Add"), this);
    add->setObjectName(QStringLiteral("hide:add"));
    add->setToolTip(QStringLiteral("Add this rule and hide what it matches"));
    addRow->addWidget(input_, 1);
    addRow->addWidget(add);
    v->addLayout(addRow);
    auto commit = [this] {
        if (addPattern(input_->text())) input_->clear();
    };
    QObject::connect(add, &QPushButton::clicked, this, commit);
    QObject::connect(input_, &QLineEdit::returnPressed, this, commit);

    v->addWidget(new QLabel(QStringLiteral("<b>Volumes in this map</b>"), this));
    classes_ = new QVBoxLayout;
    v->addLayout(classes_);
    v->addStretch(1);
    showAll_ = new QPushButton(QStringLiteral("Show all classes"), this);
    showAll_->setObjectName(QStringLiteral("hide:showAll"));
    showAll_->setToolTip(QStringLiteral("Switch every rule off (hidden brushes and entities from the outliner stay hidden)"));
    QObject::connect(showAll_, &QPushButton::clicked, this, [this] { showAllClasses(); });
    v->addWidget(showAll_, 0, Qt::AlignLeft);

    QObject::connect(&win_, &MainWindow::hiddenChanged, this, [this] { refresh(); });
    QObject::connect(&win_, &MainWindow::documentEdited, this, [this] { refresh(); });
    QObject::connect(&win_, &MainWindow::documentChanged, this, [this] { refresh(); });
    refresh();
}

QWidget* HideDialog::part(const QString& name) const {
    return findChild<QWidget*>(QStringLiteral("hide:") + name);
}

void HideDialog::refresh() {
    filling_ = true;
    const tools::HideState& hide = win_.hideState();
    const std::vector<tools::HideRule>& rules = hide.rules;
    int active = 0;
    for (const tools::HideRule& r : rules) active += r.enabled ? 1 : 0;
    summary_->setText(active ? QStringLiteral("%1 rule%2 on").arg(active).arg(active == 1 ? "" : "s") : QStringLiteral("No rule is on"));
    setWindowTitle(active ? QStringLiteral("Hide (%1)").arg(active) : QStringLiteral("Hide"));

    clearLayout(rules_);
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        auto* row = new QHBoxLayout;
        auto* box = new QCheckBox(qs(rules[i].pattern), this);
        box->setObjectName(QStringLiteral("hide:rule:%1").arg(i));
        box->setChecked(rules[i].enabled);
        QObject::connect(box, &QCheckBox::toggled, this, [this, i](bool on) {
            if (!filling_) setRuleEnabled(i, on);
        });
        row->addWidget(box, 1);
        if (rules[i].custom) {
            auto* x = new QPushButton(QStringLiteral("\xE2\x9C\x95"), this);
            x->setObjectName(QStringLiteral("hide:remove:%1").arg(i));
            x->setToolTip(QStringLiteral("Remove this rule"));
            x->setFlat(true);
            x->setFixedWidth(28);
            QObject::connect(x, &QPushButton::clicked, this, [this, i] { removeRule(i); });
            row->addWidget(x);
        }
        rules_->addLayout(row);
    }

    clearLayout(classes_);
    std::vector<tools::VolumeClass> volumes;
    if (win_.document()) volumes = tools::volumeClasses(*win_.document());
    if (volumes.empty()) {
        auto* none = new QLabel(QStringLiteral("No brush entities yet"), this);
        none->setObjectName(QStringLiteral("hide:empty"));
        classes_->addWidget(none);
    }
    for (const tools::VolumeClass& c : volumes) {
        const std::string n = tools::normalizePattern(c.classname);
        const tools::HideRule* by = tools::matchingRule(c.classname, rules);
        bool exact = false;
        for (const tools::HideRule& r : rules) exact = exact || (tools::normalizePattern(r.pattern) == n && r.enabled);
        const bool viaOther = by && !exact;   // hidden by a wider rule (trigger_*): not this box's to switch off
        auto* box = new QCheckBox(QStringLiteral("%1  \xC3\x97%2").arg(qs(c.classname)).arg(c.count), this);
        box->setObjectName(QStringLiteral("hide:class:%1").arg(qs(c.classname)));
        box->setChecked(by != nullptr);
        if (viaOther) {
            box->setEnabled(false);
            box->setToolTip(QStringLiteral("Hidden by the rule %1").arg(qs(by->pattern)));
        }
        QObject::connect(box, &QCheckBox::toggled, this, [this, name = c.classname](bool on) {
            if (!filling_) setClassHidden(name, on);
        });
        classes_->addWidget(box);
    }
    showAll_->setEnabled(active > 0);
    filling_ = false;
}

void HideDialog::setRuleEnabled(int index, bool on) {
    std::vector<tools::HideRule> rules = win_.hideState().rules;
    if (index < 0 || index >= static_cast<int>(rules.size())) return;
    rules[index].enabled = on;
    win_.setHideRules(rules);
}

void HideDialog::removeRule(int index) {
    std::vector<tools::HideRule> rules = win_.hideState().rules;
    if (index < 0 || index >= static_cast<int>(rules.size()) || !rules[index].custom) return;
    rules.erase(rules.begin() + index);
    win_.setHideRules(rules);
}

bool HideDialog::addPattern(const QString& text) {
    const std::string p = tools::normalizePattern(text.toUtf8().toStdString());
    if (p.empty()) return false;
    win_.setHideRules(tools::addRule(win_.hideState().rules, p));
    return true;
}

void HideDialog::setClassHidden(const std::string& classname, bool hidden) {
    const std::vector<tools::HideRule>& rules = win_.hideState().rules;
    const std::string n = tools::normalizePattern(classname);
    if (hidden) {
        win_.setHideRules(tools::addRule(rules, classname));
        return;
    }
    // off: the rule of exactly this class goes (a rule the user added) or is switched off (a preset)
    std::vector<tools::HideRule> out;
    for (const tools::HideRule& r : rules) {
        if (tools::normalizePattern(r.pattern) != n) out.push_back(r);
        else if (!r.custom) {
            out.push_back(r);
            out.back().enabled = false;
        }
    }
    win_.setHideRules(out);
}

void HideDialog::showAllClasses() {
    std::vector<tools::HideRule> rules = win_.hideState().rules;
    for (tools::HideRule& r : rules) r.enabled = false;
    win_.setHideRules(rules);
}

} // namespace native::ui
