// ui/details_panel -- see details_panel.h.
#include "ui/details_panel.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleValidator>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QLocale>
#include <QScrollArea>
#include <QStringListModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "doc/prefabs.h"
#include "doc/selection.h"
#include "tools/properties.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

constexpr int kLabelWidth = 96;

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString objectNameOf(const tools::DetailsRow& row) { return QStringLiteral("row:") + qs(row.label); }

QLabel* dimLabel(const QString& text, QWidget* parent = nullptr) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setEnabled(false);   // the disabled palette is the dimmed one
    return l;
}

} // namespace

DetailsPanel::DetailsPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("detailsPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll_);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { rebuild(); });
    QObject::connect(&win_, &MainWindow::documentEdited, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::documentChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::selectionChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::prefabExpansionChanged, this, [this] { scheduleRefresh(); });
    QObject::connect(&win_, &MainWindow::entityDefinitionsChanged, this, [this] { scheduleRefresh(); });
    if (qGuiApp)
        QObject::connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
            if (stale_ && !focusInside()) scheduleRefresh();   // the form catches up when the keyboard leaves it
        });
    scheduleRefresh();   // the window is still being built: it fills the form when it is up
}

DetailsPanel::~DetailsPanel() = default;

void DetailsPanel::scheduleRefresh(bool force) {
    if (force) force_ = true;
    timer_->start();
}

void DetailsPanel::refreshNow() {
    timer_->stop();
    force_ = true;
    rebuild();
}

bool DetailsPanel::focusInside() const {
    const QWidget* f = QApplication::focusWidget();
    return f && scroll_->widget() && scroll_->widget()->isAncestorOf(f);
}

std::string DetailsPanel::selectionKey() const {
    const doc::SelRef* p = win_.selection().primary();
    if (!p) return "";
    std::string k;
    for (const std::string& i : p->instancePath) k += i + "/";
    k += "|" + p->entityId + "|" + p->brushId;
    if (win_.selection().items().size() > 1) {   // the form of several entities depends on all of them
        k += "|multi";
        for (const doc::SelRef& s : win_.selection().items()) k += ":" + s.entityId + (s.instancePath.empty() ? "" : "@" + s.instancePath[0]);
    }
    return k;
}

void DetailsPanel::openOverride(const std::string& path) {
    openPath_ = openPath_ == path ? std::string() : path;
    scheduleRefresh(true);
}

void DetailsPanel::rebuild() {
    const std::shared_ptr<const doc::DocumentVersion> d = win_.document();
    const std::string selKey = selectionKey();
    std::string sig = std::to_string(d ? d->version : 0) + "|" + selKey + "|" + openPath_ + "|" + std::to_string(reinterpret_cast<uintptr_t>(win_.entityDefinitions())) + "|";
    for (const auto& [id, keys] : pendingRefs_) sig += id + ":" + std::to_string(keys.size()) + ",";
    if (const auto exp = win_.prefabExpansion()) sig += "|" + std::to_string(exp->documentVersion) + "/" + std::to_string(exp->entityCount());
    if (sig == signature_ && !force_) return;
    // Don't rebuild while the user is typing in one of our fields (they would lose the caret): the form catches up when the focus leaves.
    // A different selection always rebuilds: the focused field then belongs to something that is no longer being edited.
    if (!force_ && selKey == shownSelection_ && focusInside()) {
        stale_ = true;
        return;
    }
    force_ = false;
    stale_ = false;
    signature_ = sig;
    shownSelection_ = selKey;
    ++rebuilds_;
    build();
}

void DetailsPanel::build() {
    const std::shared_ptr<const doc::DocumentVersion> d = win_.document();
    const doc::SelRef* sel = win_.selection().primary();
    std::vector<core::ResolvedEntity> flat;
    if (const auto exp = win_.prefabExpansion()) flat = exp->flat();
    tools::DetailsContext ctx;
    ctx.doc = d;
    ctx.selection = sel;
    ctx.selections = &win_.selection().items();
    ctx.defs = win_.entityDefinitions();
    ctx.prefab = &flat;
    ctx.library = &win_.prefabLibrary();
    ctx.openPath = openPath_;
    if (d && sel && !sel->entityId.empty() && sel->instancePath.empty()) {
        auto& pending = pendingRefs_[sel->entityId];
        if (const int i = d->findEntity(sel->entityId); i >= 0)
            for (auto it = pending.begin(); it != pending.end();) it = d->entities[static_cast<size_t>(i)]->typed.refs.count(*it) ? pending.erase(it) : std::next(it);
        ctx.pendingRefs = pending;
    }
    form_ = tools::buildDetails(ctx);

    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(3);
    if (form_.mode == tools::DetailsForm::Mode::Nothing || form_.mode == tools::DetailsForm::Mode::Inside) {
        auto* l = dimLabel(qs(form_.message), content);
        l->setObjectName(QStringLiteral("detailsMessage"));
        v->addWidget(l);
    }
    for (const tools::DetailsRow& row : form_.rows) v->addWidget(makeRow(row));
    v->addStretch(1);
    scroll_->setWidget(content);   // the old form is deleted
}

QWidget* DetailsPanel::editor(const QString& label) const {
    return scroll_->widget() ? scroll_->widget()->findChild<QWidget*>(QStringLiteral("row:") + label) : nullptr;
}

bool DetailsPanel::apply(const tools::DetailsEdit& edit, const tools::EditValue& value) {
    const bool ok = win_.editDetails(edit, value);
    if (edit.kind == tools::DetailsEdit::Kind::Ref && !edit.entityId.empty() && !value.paths.empty()) pendingRefs_[edit.entityId].erase(edit.key);
    // a refused or unchanged edit leaves the field showing what was typed: show the document again
    if (!ok) scheduleRefresh(true);
    return ok;
}

// ---- rows ----------------------------------------------------------------------------------------------------------------------------

QWidget* DetailsPanel::rowWidget(const tools::DetailsRow& row, QWidget* editor, QWidget* trailing) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    auto* label = new QLabel(qs(row.label), w);
    label->setFixedWidth(kLabelWidth);
    label->setToolTip(qs(row.tip.empty() ? row.label : row.tip));
    label->setMinimumHeight(20);
    {   // long names are elided
        const QFontMetrics fm(label->font());
        label->setText(fm.elidedText(qs(row.label), Qt::ElideRight, kLabelWidth));
    }
    if (row.changed) {
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);
    }
    h->addWidget(label, 0, Qt::AlignTop);
    if (editor) {
        editor->setToolTip(label->toolTip());
        h->addWidget(editor, 1);
    }
    if (trailing) h->addWidget(trailing, 0, Qt::AlignTop);
    return w;
}

QWidget* DetailsPanel::numberEdit(const tools::DetailsRow& row, const std::string& text, const std::function<void(double)>& commit, const QString& name) {
    auto* e = new QLineEdit(qs(text));
    e->setObjectName(name);
    e->setMinimumWidth(24);
    auto* validator = new QDoubleValidator(e);
    validator->setNotation(QDoubleValidator::StandardNotation);
    validator->setLocale(QLocale::c());
    e->setValidator(validator);
    e->setEnabled(!row.readOnly);
    const std::string original = text;
    QObject::connect(e, &QLineEdit::editingFinished, e, [e, original, commit] {
        const QString t = e->text().trimmed();
        if (t.toStdString() == original) return;
        bool ok = false;
        const double v = QLocale::c().toDouble(t, &ok);
        if (!ok && t.isEmpty()) return;   // an emptied field is left alone (the value is only changed by a number)
        if (!ok) {
            e->setText(qs(original));
            return;
        }
        commit(v);
    });
    return e;
}

QWidget* DetailsPanel::vecEdit(const tools::DetailsRow& row) {
    auto* box = new QWidget;
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    for (int i = 0; i < 3; ++i) {
        const tools::DetailsRow r = row;
        h->addWidget(numberEdit(row, row.vec[static_cast<size_t>(i)], [this, r, i](double v) {
            tools::DetailsEdit e = r.edit;
            e.axis = i;
            tools::EditValue val;
            val.number = v;
            apply(e, val);
        }, objectNameOf(row) + QStringLiteral(":") + QString::number(i)));
    }
    box->setObjectName(objectNameOf(row));
    return box;
}

QWidget* DetailsPanel::refEdit(const tools::DetailsRow& row) {
    auto* box = new QWidget;
    box->setObjectName(objectNameOf(row));
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    const tools::DetailsRow r = row;
    const auto picker = [&](const std::string& value, const QString& none, const std::function<void(const std::string&)>& change, const QString& name) {
        auto* c = new QComboBox;
        c->setObjectName(name);
        c->setEnabled(!r.readOnly);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
        c->addItem(none, QString());
        bool found = value.empty();
        for (const tools::RefTarget& t : form_.targets) {
            if (t.path == r.excludePath) continue;
            c->addItem(qs((t.group.empty() ? std::string() : t.group + " / ") + t.label), qs(t.path));
            if (t.path == value) found = true;
        }
        if (!found) c->addItem(QStringLiteral("⚠ ") + qs(value) + QStringLiteral(" (missing)"), qs(value));
        c->setCurrentIndex(std::max(0, c->findData(qs(value))));
        QObject::connect(c, &QComboBox::activated, c, [c, change](int) { change(c->currentData().toString().toStdString()); });
        return c;
    };
    const auto commit = [this, r](const std::vector<std::string>& list) {
        tools::EditValue val;
        val.paths = list;
        apply(r.edit, val);
    };
    const std::vector<std::string> paths = r.paths;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto* line = new QWidget;
        auto* h = new QHBoxLayout(line);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(3);
        h->addWidget(picker(paths[i], QStringLiteral("— none —"), [paths, i, commit](const std::string& pick) {
            std::vector<std::string> next;
            for (size_t j = 0; j < paths.size(); ++j) {
                if (j != i) next.push_back(paths[j]);
                else if (!pick.empty()) next.push_back(pick);
            }
            commit(next);
        }, objectNameOf(row) + QStringLiteral(":ref") + QString::number(i)), 1);
        auto* clr = new QPushButton(QStringLiteral("×"));
        clr->setObjectName(objectNameOf(row) + QStringLiteral(":remove") + QString::number(i));
        clr->setFixedWidth(24);
        clr->setToolTip(QString::fromLatin1(paths.size() > 1 ? "Remove this target" : "Clear / revert"));
        clr->setEnabled(!r.readOnly);
        QObject::connect(clr, &QPushButton::clicked, clr, [paths, i, commit] {
            std::vector<std::string> next;
            for (size_t j = 0; j < paths.size(); ++j)
                if (j != i) next.push_back(paths[j]);
            commit(next);
        });
        h->addWidget(clr);
        v->addWidget(line);
    }
    v->addWidget(picker("", paths.empty() ? QStringLiteral("— none —") : QStringLiteral("+ add a target"), [paths, commit](const std::string& pick) {
        if (pick.empty()) return;
        std::vector<std::string> next = paths;
        next.push_back(pick);
        commit(next);
    }, objectNameOf(row) + QStringLiteral(":add")));
    if (!r.warning.empty()) {
        auto* warn = dimLabel(qs(r.warning), box);
        warn->setObjectName(objectNameOf(row) + QStringLiteral(":warning"));
        v->addWidget(warn);
        auto* share = new QPushButton(QStringLiteral("Share first name"));
        share->setObjectName(objectNameOf(row) + QStringLiteral(":share"));
        share->setToolTip(QStringLiteral("Give every target of this document the name of the first"));
        share->setEnabled(r.canShare);
        QObject::connect(share, &QPushButton::clicked, share, [this, paths] {
            tools::DetailsEdit e;
            e.kind = tools::DetailsEdit::Kind::ShareName;
            tools::EditValue val;
            val.paths = paths;
            apply(e, val);
        });
        v->addWidget(share);
    }
    return box;
}

QWidget* DetailsPanel::addKeyRow(const tools::DetailsRow& row) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 4, 0, 0);
    h->setSpacing(3);
    auto* key = new QLineEdit;
    key->setObjectName(QStringLiteral("addkey:key"));
    key->setMinimumWidth(30);
    key->setPlaceholderText(QStringLiteral("new key"));
    auto* completer = new QCompleter(key);
    QStringList refKeys;
    for (const std::string& k : form_.refKeys) refKeys << qs(k);
    completer->setModel(new QStringListModel(refKeys, completer));
    key->setCompleter(completer);
    auto* value = new QLineEdit;
    value->setObjectName(QStringLiteral("addkey:value"));
    value->setMinimumWidth(30);
    value->setPlaceholderText(QStringLiteral("value"));
    auto* add = new QPushButton(QStringLiteral("Add"));
    add->setObjectName(QStringLiteral("addkey:button"));
    for (QWidget* x : {static_cast<QWidget*>(key), static_cast<QWidget*>(value), static_cast<QWidget*>(add)}) x->setEnabled(!row.readOnly);
    h->addWidget(key, 1);
    h->addWidget(value, 1);
    h->addWidget(add);
    const tools::DetailsRow r = row;
    const std::vector<std::string> refKeys2 = form_.refKeys;
    QObject::connect(add, &QPushButton::clicked, add, [this, r, key, value, refKeys2, add] {
        const std::string k = key->text().trimmed().toStdString();
        if (k.empty()) return;
        const std::string val = value->text().toStdString();
        if (r.edit.kind == tools::DetailsEdit::Kind::Keyvalue && val.empty() && std::find(refKeys2.begin(), refKeys2.end(), k) != refKeys2.end()) {
            pendingRefs_[r.edit.entityId].insert(k);   // a reference key without a value: an empty picker row instead of a text value
            add->clearFocus();
            scheduleRefresh(true);
            return;
        }
        tools::DetailsEdit e = r.edit;
        e.key = k;
        tools::EditValue ev;
        ev.text = val;
        apply(e, ev);
    });
    return w;
}

QWidget* DetailsPanel::makeRow(const tools::DetailsRow& row) {
    using tools::RowKind;
    switch (row.kind) {
        case RowKind::Section: {
            auto* l = new QLabel(qs(row.label));
            QFont f = l->font();
            f.setBold(true);
            l->setFont(f);
            l->setContentsMargins(0, 8, 0, 2);
            return l;
        }
        case RowKind::Note: {
            auto* l = dimLabel(qs(row.label));
            l->setObjectName(QStringLiteral("note"));
            return l;
        }
        case RowKind::Static: {
            auto* e = new QLineEdit(qs(row.value));
            e->setObjectName(objectNameOf(row));
            e->setReadOnly(true);
            e->setEnabled(false);
            return rowWidget(row, e);
        }
        case RowKind::Text:
        case RowKind::Asset: {
            auto* e = new QLineEdit(qs(row.value));
            e->setObjectName(objectNameOf(row));
            e->setMinimumWidth(40);
            e->setEnabled(!row.readOnly);
            if (row.kind == RowKind::Text && row.value.empty() && row.edit.emptyRemoves) e->setPlaceholderText(row.label == "targetname" ? QStringLiteral("no name") : QStringLiteral("unset"));
            const tools::DetailsRow r = row;
            QObject::connect(e, &QLineEdit::editingFinished, e, [this, r, e] {
                if (e->text().toStdString() == r.value) return;
                tools::EditValue v;
                v.text = e->text().toStdString();
                apply(r.edit, v);
            });
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* b = new QPushButton(QStringLiteral("↺"));
                b->setObjectName(QStringLiteral("reset:") + qs(row.label));
                b->setFixedWidth(24);
                b->setToolTip(qs(row.resetLabel));
                b->setEnabled(row.resettable);
                QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
                reset = b;
            }
            return rowWidget(row, e, reset);
        }
        case RowKind::Real:
        case RowKind::Integer: {
            const tools::DetailsRow r = row;
            QWidget* e = numberEdit(row, row.value, [this, r](double v) {
                tools::EditValue val;
                val.number = v;
                apply(r.edit, val);
            }, objectNameOf(row));
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* b = new QPushButton(QStringLiteral("↺"));
                b->setObjectName(QStringLiteral("reset:") + qs(row.label));
                b->setFixedWidth(24);
                b->setToolTip(qs(row.resetLabel));
                b->setEnabled(row.resettable);
                QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
                reset = b;
            }
            return rowWidget(row, e, reset);
        }
        case RowKind::Vec3: {
            QWidget* e = vecEdit(row);
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* b = new QPushButton(QStringLiteral("↺"));
                b->setObjectName(QStringLiteral("reset:") + qs(row.label));
                b->setFixedWidth(24);
                b->setToolTip(qs(row.resetLabel));
                b->setEnabled(row.resettable);
                const tools::DetailsRow r = row;
                QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
                reset = b;
            }
            return rowWidget(row, e, reset);
        }
        case RowKind::Check: {
            auto* c = new QCheckBox;
            c->setObjectName(objectNameOf(row));
            c->setChecked(row.checked);
            c->setEnabled(!row.readOnly);
            const tools::DetailsRow r = row;
            QObject::connect(c, &QCheckBox::clicked, c, [this, r](bool on) {
                tools::EditValue v;
                v.checked = on;
                apply(r.edit, v);
            });
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* b = new QPushButton(QStringLiteral("↺"));
                b->setObjectName(QStringLiteral("reset:") + qs(row.label));
                b->setFixedWidth(24);
                b->setToolTip(qs(row.resetLabel));
                b->setEnabled(row.resettable);
                QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
                reset = b;
            }
            auto* w = rowWidget(row, c, reset);
            return w;
        }
        case RowKind::Choice: {
            auto* c = new QComboBox;
            c->setObjectName(objectNameOf(row));
            c->setEnabled(!row.readOnly);
            for (const auto& [name, value] : row.choices) c->addItem(qs(name), qs(value));
            c->setCurrentIndex(std::max(0, c->findData(qs(row.value))));
            const tools::DetailsRow r = row;
            QObject::connect(c, &QComboBox::activated, c, [this, r, c](int) {
                tools::EditValue v;
                v.text = c->currentData().toString().toStdString();
                v.checked = v.text == "subtract";   // the brush operation row
                apply(r.edit, v);
            });
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* b = new QPushButton(QStringLiteral("↺"));
                b->setObjectName(QStringLiteral("reset:") + qs(row.label));
                b->setFixedWidth(24);
                b->setToolTip(qs(row.resetLabel));
                b->setEnabled(row.resettable);
                QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
                reset = b;
            }
            return rowWidget(row, c, reset);
        }
        case RowKind::Color: {
            auto* b = new QPushButton;
            b->setObjectName(objectNameOf(row));
            b->setEnabled(!row.readOnly);
            const QString hex = qs(row.vec[0]);
            b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #666; min-height: 18px;").arg(hex));
            b->setToolTip(qs(row.label + ": " + row.value));
            const tools::DetailsRow r = row;
            QObject::connect(b, &QPushButton::clicked, b, [this, r, hex] {
                const QColor c = QColorDialog::getColor(QColor(hex), this, qs(r.label));
                if (!c.isValid()) return;
                tools::EditValue v;
                v.text = c.name().toStdString();
                apply(r.edit, v);
            });
            QWidget* reset = nullptr;
            if (row.reset.kind != tools::DetailsEdit::Kind::None) {
                auto* rb = new QPushButton(QStringLiteral("↺"));
                rb->setObjectName(QStringLiteral("reset:") + qs(row.label));
                rb->setFixedWidth(24);
                rb->setToolTip(qs(row.resetLabel));
                rb->setEnabled(row.resettable);
                QObject::connect(rb, &QPushButton::clicked, rb, [this, r] { apply(r.reset, {}); });
                reset = rb;
            }
            return rowWidget(row, b, reset);
        }
        case RowKind::Ref: return rowWidget(row, refEdit(row));
        case RowKind::KeyValue: {
            auto* e = new QLineEdit(qs(row.value));
            e->setObjectName(objectNameOf(row));
            e->setMinimumWidth(40);
            e->setEnabled(!row.readOnly);
            if (row.mixed) e->setPlaceholderText(QString::fromUtf8("â (differs)"));
            const tools::DetailsRow r = row;
            QObject::connect(e, &QLineEdit::editingFinished, e, [this, r, e] {
                if (e->text().toStdString() == r.value) return;
                tools::EditValue v;
                v.text = e->text().toStdString();
                apply(r.edit, v);
            });
            auto* b = new QPushButton(r.edit.kind == tools::DetailsEdit::Kind::OverrideKeyvalue ? QStringLiteral("↺") : QStringLiteral("×"));
            b->setObjectName(QStringLiteral("reset:") + qs(row.label));
            b->setFixedWidth(24);
            b->setToolTip(r.edit.kind == tools::DetailsEdit::Kind::OverrideKeyvalue ? QStringLiteral("Revert to the prefab value") : QStringLiteral("Remove"));
            b->setEnabled(row.resettable);
            QObject::connect(b, &QPushButton::clicked, b, [this, r] { apply(r.reset, {}); });
            return rowWidget(row, e, b);
        }
        case RowKind::AddKey: return addKeyRow(row);
        case RowKind::Content: {
            auto* b = new QPushButton(QString::fromUtf8(row.open ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") + qs(row.label) + QStringLiteral("   ") + qs(row.value));
            b->setObjectName(objectNameOf(row));
            b->setFlat(true);
            b->setStyleSheet(QStringLiteral("text-align: left; padding: 3px;"));
            const std::string path = row.path;
            QObject::connect(b, &QPushButton::clicked, b, [this, path] { openOverride(path); });
            return b;
        }
        case RowKind::Summary: {
            auto* w = new QWidget;
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);
            const bool isDelete = row.reset.kind == tools::DetailsEdit::Kind::OverrideDelete;
            if (!isDelete) {
                auto* l = new QLabel(qs(row.label));
                l->setToolTip(qs(row.path));
                h->addWidget(l);
                h->addWidget(dimLabel(qs(row.value)), 1);
            } else {
                h->addStretch(1);
            }
            auto* b = new QPushButton(qs(isDelete ? row.label : row.resetLabel));
            b->setObjectName(QStringLiteral("reset:") + qs(row.label) + (isDelete ? QStringLiteral(":") + qs(row.path) : QString()));
            b->setToolTip(qs(row.tip));
            b->setEnabled(!row.readOnly);
            const tools::DetailsRow r = row;
            QObject::connect(b, &QPushButton::clicked, b, [this, r, isDelete] {
                tools::EditValue v;
                v.checked = true;
                if (isDelete) openPath_.clear();
                apply(r.reset, v);
            });
            h->addWidget(b);
            return w;
        }
    }
    return new QWidget;
}

} // namespace native::ui
