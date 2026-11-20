// ui/keymap_page -- see keymap_page.h.
#include "ui/keymap_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/chord_capture.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }
std::string str(const QString& s) { return s.toUtf8().toStdString(); }

QPushButton* button(const QString& text, const char* name, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(QString::fromLatin1(name));
    return b;
}
} // namespace

// ---- the recorder ---------------------------------------------------------------------------------------------------------------

BindingRecorder::BindingRecorder(MainWindow& window, const actions::ActionDef& def, QWidget* parent)
    : QDialog(parent ? parent : &window), win_(window), def_(def) {
    setObjectName(QStringLiteral("bindingRecorder"));
    setWindowTitle(QStringLiteral("New binding"));
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("<b>%1</b><br>%2").arg(qs(def.label).toHtmlEscaped(), qs(def.description).toHtmlEscaped()), this));

    capture_ = new ChordCapture(this);
    capture_->setObjectName(QStringLiteral("keymap:rec:input"));
    root->addWidget(capture_);

    auto* options = new QHBoxLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("keymap:rec:mode"));
    mode_->addItem(QStringLiteral("Press"), static_cast<int>(actions::ChordMode::Press));
    mode_->addItem(QStringLiteral("Hold"), static_cast<int>(actions::ChordMode::Hold));
    mode_->addItem(QStringLiteral("Release"), static_cast<int>(actions::ChordMode::Release));
    mode_->addItem(QStringLiteral("Double tap"), static_cast<int>(actions::ChordMode::DoubleTap));
    mode_->setToolTip(QStringLiteral("How a key triggers: when pressed, while held, when released, or on a double tap. Mouse buttons and the wheel are always plain."));
    physical_ = new QCheckBox(QStringLiteral("By position"), this);
    physical_->setObjectName(QStringLiteral("keymap:rec:physical"));
    physical_->setToolTip(QStringLiteral("Match the key by where it sits on the keyboard (WASD stays put on any layout) instead of the character it types."));
    typeInstead_ = new QCheckBox(QStringLiteral("Type it"), this);
    typeInstead_->setObjectName(QStringLiteral("keymap:rec:type"));
    typeInstead_->setToolTip(QStringLiteral("Type the input as text, for example a sequence: Ctrl+K, Ctrl+S"));
    options->addWidget(mode_);
    options->addWidget(physical_);
    options->addWidget(typeInstead_);
    options->addStretch(1);
    root->addLayout(options);

    auto* form = new QGridLayout;
    form->addWidget(new QLabel(QStringLiteral("Context"), this), 0, 0);
    context_ = new QComboBox(this);
    context_->setObjectName(QStringLiteral("keymap:rec:context"));
    for (const std::string& c : actions::knownContexts(win_.registry(), win_.keymap())) context_->addItem(qs(c), qs(c));
    setContext(actions::defaultContextFor(def));
    context_->setToolTip(QStringLiteral("Where the binding works; a more specific context wins over a general one."));
    form->addWidget(context_, 0, 1);
    form->addWidget(new QLabel(QStringLiteral("Parameters"), this), 1, 0);
    params_ = new QLineEdit(this);
    params_->setObjectName(QStringLiteral("keymap:rec:params"));
    params_->setPlaceholderText(QStringLiteral("none"));
    if (!def.params.is_null()) params_->setText(qs(def.params.dump()));
    params_->setToolTip(QStringLiteral("The action's parameters as JSON, for example {\"size\": 0.25}"));
    form->addWidget(params_, 1, 1);
    root->addLayout(form);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("keymap:rec:status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setObjectName(QStringLiteral("keymap:rec:ok"));
    ok_->setText(QStringLiteral("Add"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    QObject::connect(mode_, &QComboBox::currentIndexChanged, this, [this] { capture_->setMode(static_cast<actions::ChordMode>(mode_->currentData().toInt())); });
    QObject::connect(physical_, &QCheckBox::toggled, this, [this](bool on) { capture_->setPhysical(on); });
    QObject::connect(typeInstead_, &QCheckBox::toggled, this, [this](bool on) {
        capture_->setCapturing(!on);
        if (on) capture_->setFocus();
    });
    QObject::connect(capture_, &QLineEdit::textChanged, this, [this] { refresh(); });
    QObject::connect(context_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    QObject::connect(params_, &QLineEdit::textChanged, this, [this] { refresh(); });
    refresh();
    capture_->setFocus();
}

void BindingRecorder::setContext(const std::string& context) {
    int i = context_->findData(qs(context));
    if (i < 0) {
        context_->addItem(qs(context), qs(context));
        i = context_->count() - 1;
    }
    context_->setCurrentIndex(i);
}

void BindingRecorder::setInput(const QString& text) { capture_->setText(text); }

BindingRecorder::Entry BindingRecorder::entry() const {
    Entry e;
    const std::string input = str(capture_->text().trimmed());
    if (input.empty()) {
        e.error = "Press the input to bind.";
        return e;
    }
    nlohmann::json params = nullptr;
    const std::string ptext = str(params_->text().trimmed());
    if (!ptext.empty()) {
        params = nlohmann::json::parse(ptext, nullptr, false);
        if (params.is_discarded() || !params.is_object()) {
            e.error = "The parameters must be a JSON object, for example {\"size\": 0.25}.";
            return e;
        }
    }
    std::string err;
    if (!actions::makeBinding(def_.id, str(context_->currentData().toString()), input, params, false, e.binding, &err)) {
        e.error = err;
        return e;
    }
    if (!actions::bindableChord(e.binding.chord, &err)) e.error = err;
    return e;
}

void BindingRecorder::refresh() {
    const Entry e = entry();
    QString text;
    if (!e.error.empty()) {
        text = qs(e.error);
    } else {
        const auto conflicts = actions::conflictsFor(win_.registry(), win_.keymap(), e.binding);
        if (conflicts.empty()) text = QStringLiteral("Free: nothing else uses this input there.");
        else {
            text = qs(actions::describeConflict(conflicts.front()));
            if (conflicts.size() > 1) text += QStringLiteral(" (and %1 more)").arg(conflicts.size() - 1);
        }
    }
    status_->setText(text);
    ok_->setEnabled(e.error.empty());
}

// ---- the page ------------------------------------------------------------------------------------------------------------------

KeymapPage::KeymapPage(MainWindow& window, QWidget* parent)
    : QWidget(parent), win_(window), editor_(std::make_unique<actions::BindingEditor>(window.registry(), window.keymap())) {
    setObjectName(QStringLiteral("keymapPage"));
    auto* root = new QVBoxLayout(this);

    // the user keymaps
    auto* files = new QHBoxLayout;
    files->addWidget(new QLabel(QStringLiteral("User keymap"), this));
    keymaps_ = new QComboBox(this);
    keymaps_->setObjectName(QStringLiteral("keymap:active"));
    keymaps_->setMinimumWidth(140);
    new_ = button(QStringLiteral("New..."), "keymap:new", this);
    delete_ = button(QStringLiteral("Delete"), "keymap:delete", this);
    import_ = button(QStringLiteral("Import..."), "keymap:import", this);
    export_ = button(QStringLiteral("Export..."), "keymap:export", this);
    files->addWidget(keymaps_);
    files->addWidget(new_);
    files->addWidget(delete_);
    files->addWidget(import_);
    files->addWidget(export_);
    files->addStretch(1);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("keymap:note"));
    files->addWidget(note_);
    root->addLayout(files);

    // filters
    auto* filters = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("keymap:search"));
    search_->setPlaceholderText(QStringLiteral("Search actions and keys"));
    search_->setClearButtonEnabled(true);
    show_ = new QComboBox(this);
    show_->setObjectName(QStringLiteral("keymap:filter"));
    for (const char* t : {"All", "Changed", "Unbound", "Conflicts"}) show_->addItem(QString::fromLatin1(t));
    category_ = new QComboBox(this);
    category_->setObjectName(QStringLiteral("keymap:category"));
    category_->addItem(QStringLiteral("Any category"), QString());
    for (const std::string& c : actions::categories(win_.registry())) category_->addItem(qs(c), qs(c));
    context_ = new QComboBox(this);
    context_->setObjectName(QStringLiteral("keymap:context"));
    filters->addWidget(search_, 1);
    filters->addWidget(show_);
    filters->addWidget(category_);
    filters->addWidget(context_);
    root->addLayout(filters);

    table_ = new QTreeWidget(this);
    table_->setObjectName(QStringLiteral("keymap:table"));
    table_->setColumnCount(4);
    table_->setHeaderLabels({QStringLiteral("Category"), QStringLiteral("Action"), QStringLiteral("Bindings"), QStringLiteral("Source")});
    table_->setRootIsDecorated(false);
    table_->setUniformRowHeights(true);
    table_->setAlternatingRowColors(true);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->header()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setColumnWidth(0, 90);
    table_->setColumnWidth(1, 170);
    table_->setColumnWidth(2, 300);
    root->addWidget(table_, 1);

    description_ = new QLabel(this);
    description_->setObjectName(QStringLiteral("keymap:description"));
    description_->setWordWrap(true);
    root->addWidget(description_);

    auto* detail = new QHBoxLayout;
    bindings_ = new QListWidget(this);
    bindings_->setObjectName(QStringLiteral("keymap:bindings"));
    bindings_->setMaximumHeight(84);
    detail->addWidget(bindings_, 1);
    auto* column = new QVBoxLayout;
    add_ = button(QStringLiteral("Add binding..."), "keymap:add", this);
    remove_ = button(QStringLiteral("Remove binding"), "keymap:remove", this);
    resetAction_ = button(QStringLiteral("Reset action"), "keymap:resetAction", this);
    column->addWidget(add_);
    column->addWidget(remove_);
    column->addWidget(resetAction_);
    detail->addLayout(column);
    root->addLayout(detail);

    auto* bottom = new QHBoxLayout;
    undo_ = button(QStringLiteral("Undo"), "keymap:undo", this);
    redo_ = button(QStringLiteral("Redo"), "keymap:redo", this);
    resetAll_ = button(QStringLiteral("Reset all"), "keymap:resetAll", this);
    bottom->addWidget(undo_);
    bottom->addWidget(redo_);
    bottom->addWidget(resetAll_);
    bottom->addSpacing(16);
    bottom->addWidget(new QLabel(QStringLiteral("What does this input do?"), this));
    lookupCapture_ = new ChordCapture(this);
    lookupCapture_->setObjectName(QStringLiteral("keymap:lookup"));
    lookupCapture_->setMinimumWidth(150);
    bottom->addWidget(lookupCapture_);
    lookupResult_ = new QLabel(this);
    lookupResult_->setObjectName(QStringLiteral("keymap:lookupResult"));
    lookupResult_->setWordWrap(true);
    bottom->addWidget(lookupResult_, 1);
    root->addLayout(bottom);

    QObject::connect(search_, &QLineEdit::textChanged, this, [this] { filterChanged(); });
    for (QComboBox* c : {show_, category_, context_}) QObject::connect(c, &QComboBox::currentIndexChanged, this, [this] { filterChanged(); });
    QObject::connect(table_, &QTreeWidget::currentItemChanged, this, [this] { fillBindings(); });
    QObject::connect(table_, &QTreeWidget::itemDoubleClicked, this, [this] { addForSelected(); });
    QObject::connect(bindings_, &QListWidget::currentRowChanged, this, [this] { remove_->setEnabled(bindings_->currentRow() >= 0); });
    QObject::connect(add_, &QPushButton::clicked, this, [this] { addForSelected(); });
    QObject::connect(remove_, &QPushButton::clicked, this, [this] { removeSelectedBinding(); });
    QObject::connect(resetAction_, &QPushButton::clicked, this, [this] {
        const std::string id = selectedAction();
        if (!id.empty() && editor_->resetAction(id)) edited();
    });
    QObject::connect(resetAll_, &QPushButton::clicked, this, [this] { if (editor_->resetAll()) edited(); });
    QObject::connect(undo_, &QPushButton::clicked, this, [this] { if (editor_->undo()) edited(); });
    QObject::connect(redo_, &QPushButton::clicked, this, [this] { if (editor_->redo()) edited(); });
    QObject::connect(lookupCapture_, &ChordCapture::captured, this, [this](const QString& c) { lookup(c); });
    QObject::connect(keymaps_, &QComboBox::activated, this, [this](int) {
        const std::string err = win_.switchKeymap(str(keymaps_->currentText()));
        if (!err.empty()) QMessageBox::warning(this, QStringLiteral("Keymap"), qs(err));
        refresh();
    });
    QObject::connect(new_, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New keymap"), QStringLiteral("Name"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok) return;
        if (const std::string err = createKeymap(str(name)); !err.empty()) QMessageBox::warning(this, QStringLiteral("Keymap"), qs(err));
    });
    QObject::connect(delete_, &QPushButton::clicked, this, [this] {
        const std::string name = win_.keymapName();
        if (!win_.confirm(QStringLiteral("Delete keymap"), QStringLiteral("Delete the keymap \"%1\" and its file?").arg(qs(name)))) return;
        if (const std::string err = removeKeymap(name); !err.empty()) QMessageBox::warning(this, QStringLiteral("Keymap"), qs(err));
    });
    QObject::connect(import_, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import keymap"), QString(), QStringLiteral("Keymaps (*.json);;All files (*)"));
        if (file.isEmpty()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Import keymap"), QStringLiteral("Name"), QLineEdit::Normal, QFileInfo(file).completeBaseName(), &ok).trimmed();
        if (!ok) return;
        if (const std::string err = importKeymap(str(file), str(name)); !err.empty()) QMessageBox::warning(this, QStringLiteral("Keymap"), qs(err));
    });
    QObject::connect(export_, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Export keymap"), qs(win_.keymapName()) + QStringLiteral(".json"),
                                                          QStringLiteral("Keymaps (*.json)"));
        if (file.isEmpty()) return;
        if (const std::string err = exportKeymap(str(file)); !err.empty()) QMessageBox::warning(this, QStringLiteral("Keymap"), qs(err));
    });
    // a keymap that changed for another reason (another user keymap, an import) starts a new edit history
    QObject::connect(&win_, &MainWindow::keymapChanged, this, [this] {
        if (ownChange_) return;
        editor_ = std::make_unique<actions::BindingEditor>(win_.registry(), win_.keymap());
        refresh();
    });
    refresh();
}

actions::RowFilter KeymapPage::filter() const {
    actions::RowFilter f;
    f.text = str(search_->text());
    f.changed = show_->currentIndex() == 1;
    f.unbound = show_->currentIndex() == 2;
    f.conflicts = show_->currentIndex() == 3;
    f.category = str(category_->currentData().toString());
    f.context = str(context_->currentData().toString());
    return f;
}

void KeymapPage::filterChanged() {
    if (!refreshing_) refresh();
}

std::string KeymapPage::selectedAction() const {
    const QTreeWidgetItem* item = table_->currentItem();
    return item ? str(item->data(0, Qt::UserRole).toString()) : std::string();
}

std::vector<std::string> KeymapPage::shownActions() const {
    std::vector<std::string> out;
    for (int i = 0; i < table_->topLevelItemCount(); ++i) out.push_back(str(table_->topLevelItem(i)->data(0, Qt::UserRole).toString()));
    return out;
}

void KeymapPage::selectAction(const std::string& id) {
    for (int i = 0; i < table_->topLevelItemCount(); ++i)
        if (str(table_->topLevelItem(i)->data(0, Qt::UserRole).toString()) == id) {
            table_->setCurrentItem(table_->topLevelItem(i));
            table_->scrollToItem(table_->topLevelItem(i));
            return;
        }
}

void KeymapPage::refresh() {
    if (refreshing_) return;
    refreshing_ = true;
    const std::string keep = selectedAction();

    // the context filter follows the contexts that exist now
    {
        const QString current = context_->currentData().toString();
        context_->clear();
        context_->addItem(QStringLiteral("Any context"), QString());
        for (const std::string& c : actions::knownContexts(win_.registry(), win_.keymap())) context_->addItem(qs(c), qs(c));
        const int i = context_->findData(current);
        context_->setCurrentIndex(i < 0 ? 0 : i);
    }
    // the user keymaps
    keymaps_->clear();
    for (const std::string& n : win_.keymapNames()) keymaps_->addItem(qs(n));
    const int active = keymaps_->findText(qs(win_.keymapName()));
    if (active >= 0) keymaps_->setCurrentIndex(active);
    const bool files = win_.hasKeymapFiles();
    for (QWidget* w : std::initializer_list<QWidget*>{keymaps_, new_, delete_, import_}) w->setEnabled(files);
    delete_->setEnabled(files && win_.keymapName() != "default");
    note_->setText(files ? QStringLiteral("Preset: %1").arg(qs(win_.keymap().presetName())) : QStringLiteral("Changes are not kept: there is no keymap folder."));

    table_->setUpdatesEnabled(false);
    table_->clear();
    QTreeWidgetItem* select = nullptr;
    for (const actions::ActionRow& row : actions::buildRows(win_.registry(), win_.keymap(), filter())) {
        QStringList chips;
        bool anyUser = false;
        for (const actions::Binding& b : row.bindings) {
            chips << qs(actions::bindingText(b));
            anyUser = anyUser || b.fromUser;
        }
        const QString source = row.changed ? QStringLiteral("Changed") : (row.unbound ? QStringLiteral("Unbound") : QStringLiteral("Preset"));
        auto* item = new QTreeWidgetItem(table_, {qs(row.def->category), qs(row.def->label), chips.join(QStringLiteral("   ")), source});
        item->setData(0, Qt::UserRole, qs(row.def->id));
        item->setToolTip(1, qs(row.def->id));
        if (row.conflicting) item->setForeground(2, QColor(0xd0, 0x40, 0x40));
        if (row.def->id == keep) select = item;
        (void)anyUser;
    }
    table_->setUpdatesEnabled(true);
    refreshing_ = false;
    if (select) table_->setCurrentItem(select);
    else if (table_->topLevelItemCount() > 0) table_->setCurrentItem(table_->topLevelItem(0));
    fillBindings();
    undo_->setEnabled(editor_->canUndo());
    redo_->setEnabled(editor_->canRedo());
    resetAll_->setEnabled(!win_.keymap().userEmpty());
}

void KeymapPage::fillBindings() {
    if (refreshing_) return;
    bindings_->clear();
    listed_.clear();
    const std::string id = selectedAction();
    const actions::ActionDef* def = id.empty() ? nullptr : win_.registry().find(id);
    add_->setEnabled(def != nullptr);
    resetAction_->setEnabled(def != nullptr);
    remove_->setEnabled(false);
    if (!def) {
        description_->setText(QString());
        return;
    }
    description_->setText(qs(def->description.empty() ? def->label : def->description));
    for (const actions::Binding* b : win_.keymap().bindingsFor(id)) {
        listed_.push_back(*b);
        QString text = qs(actions::bindingText(*b));
        if (b->fromUser) text += QStringLiteral("  [yours]");
        bindings_->addItem(text);
    }
    if (listed_.empty()) bindings_->addItem(QStringLiteral("(unbound)"));
}

void KeymapPage::edited() {
    ownChange_ = true;
    win_.keymapEdited();
    ownChange_ = false;
    refresh();
}

actions::BindingEditor::Result KeymapPage::addBinding(const actions::Binding& binding) {
    const int before = editor_->revision();
    actions::BindingEditor::Result r = editor_->add(binding, actions::BindingEditor::Resolve::Cancel);
    if (!r.ok && !r.conflicts.empty()) {
        actions::BindingEditor::Resolve choice = actions::BindingEditor::Resolve::Cancel;
        if (conflictHandler_) {
            choice = conflictHandler_(r.conflicts);
        } else {
            QMessageBox box(QMessageBox::Question, QStringLiteral("Input already used"), qs(actions::describeConflict(r.conflicts.front())), QMessageBox::NoButton, this);
            if (r.conflicts.size() > 1) box.setInformativeText(QStringLiteral("and %1 more binding(s)").arg(r.conflicts.size() - 1));
            QPushButton* replace = box.addButton(QStringLiteral("Replace"), QMessageBox::AcceptRole);
            QPushButton* both = actions::BindingEditor::canKeepBoth(r.conflicts) ? box.addButton(QStringLiteral("Keep both"), QMessageBox::ActionRole) : nullptr;
            box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == replace) choice = actions::BindingEditor::Resolve::Replace;
            else if (box.clickedButton() == both) choice = actions::BindingEditor::Resolve::KeepBoth;
        }
        if (choice == actions::BindingEditor::Resolve::Cancel) return r;
        r = editor_->add(binding, choice);
    }
    if (editor_->revision() != before) edited();
    return r;
}

bool KeymapPage::removeBinding(const actions::Binding& binding) {
    if (!editor_->remove(binding)) return false;
    edited();
    return true;
}

void KeymapPage::addForSelected() {
    const std::string id = selectedAction();
    const actions::ActionDef* def = id.empty() ? nullptr : win_.registry().find(id);
    if (!def) return;
    BindingRecorder rec(win_, *def, this);
    if (rec.exec() != QDialog::Accepted) return;
    const BindingRecorder::Entry e = rec.entry();
    if (!e.error.empty()) return;
    const auto r = addBinding(e.binding);
    if (!r.ok && r.conflicts.empty()) QMessageBox::warning(this, QStringLiteral("Binding"), qs(r.error));
}

void KeymapPage::removeSelectedBinding() {
    const int row = bindings_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= listed_.size()) return;
    removeBinding(listed_[static_cast<size_t>(row)]);
}

std::string KeymapPage::createKeymap(const std::string& name) {
    ownChange_ = true;
    const std::string err = win_.newKeymap(name);
    ownChange_ = false;
    if (err.empty()) editor_ = std::make_unique<actions::BindingEditor>(win_.registry(), win_.keymap());
    refresh();
    return err;
}

std::string KeymapPage::removeKeymap(const std::string& name) {
    ownChange_ = true;
    const std::string err = win_.deleteKeymap(name);
    ownChange_ = false;
    editor_ = std::make_unique<actions::BindingEditor>(win_.registry(), win_.keymap());
    refresh();
    return err;
}

std::string KeymapPage::importKeymap(const std::string& file, const std::string& name) {
    ownChange_ = true;
    const std::string err = win_.importKeymap(file, name);
    ownChange_ = false;
    if (err.empty()) editor_ = std::make_unique<actions::BindingEditor>(win_.registry(), win_.keymap());
    refresh();
    return err;
}

std::string KeymapPage::exportKeymap(const std::string& file) { return win_.exportKeymap(file); }

void KeymapPage::lookup(const QString& chord) {
    if (chord.trimmed().isEmpty()) {
        lookupResult_->setText(QString());
        return;
    }
    actions::Chord parsed;
    if (!actions::parseChord(str(chord), parsed)) {
        lookupResult_->setText(QStringLiteral("Not a valid input."));
        return;
    }
    const auto found = actions::lookupInput(win_.registry(), win_.keymap(), str(chord));
    if (found.empty()) {
        lookupResult_->setText(QStringLiteral("Nothing is bound to %1.").arg(qs(actions::formatChord(parsed))));
        return;
    }
    QStringList parts;
    for (const actions::Lookup& l : found) parts << QStringLiteral("%1 (%2)").arg(qs(l.label), qs(l.context));
    lookupResult_->setText(parts.join(QStringLiteral("; ")));
}

} // namespace native::ui
