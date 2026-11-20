// ui/settings_dialog -- see settings_dialog.h.
#include "ui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

#include <core/entdef.h>

#include "render/viewport_window.h"
#include "ui/main_window.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }
std::string str(const QString& s) { return s.toUtf8().toStdString(); }

QString numberText(double v) {
    return QString::number(v, 'g', 6);
}

// A combo of numbers: the list's values, the shown value selected (added when it is not in the list).
void fillNumbers(QComboBox* box, const std::vector<double>& values, const QString& suffix) {
    box->clear();
    for (const double v : values) box->addItem(numberText(v) + suffix, v);
}
void selectNumber(QComboBox* box, double v, const QString& suffix) {
    int i = box->findData(v);
    if (i < 0) {
        box->addItem(numberText(v) + suffix, v);
        i = box->count() - 1;
    }
    box->setCurrentIndex(i);
}

QLabel* note(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QLabel* section(const QString& text, QWidget* parent) {
    auto* l = new QLabel(QStringLiteral("<b>%1</b>").arg(text), parent);
    return l;
}

QString cleanFolder(const QString& s) { return QDir::cleanPath(s.trimmed()); }

} // namespace

SettingsDialog::SettingsDialog(MainWindow& window, QWidget* parent) : QDialog(parent ? parent : &window), win_(window) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("Settings"));
    resize(720, 520);

    pages_ = new QListWidget(this);
    pages_->setObjectName(QStringLiteral("settings:pages"));
    pages_->setFixedWidth(150);
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("settings:stack"));

    const std::pair<const char*, void (SettingsDialog::*)(QWidget*)> pages[] = {
        {"General", &SettingsDialog::buildGeneral}, {"Game profile", &SettingsDialog::buildProfile}, {"Editor", &SettingsDialog::buildEditor}};
    for (const auto& [title, build] : pages) {
        pages_->addItem(QString::fromLatin1(title));
        auto* page = new QWidget(stack_);
        (this->*build)(page);
        stack_->addWidget(page);
    }
    QObject::connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("settings:error"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d04040;"));
    error_->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("settings:ok"));
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("settings:apply"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("settings:cancel"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (applyAll().isEmpty()) accept();
    });
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { applyAll(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* body = new QHBoxLayout;
    body->addWidget(pages_);
    body->addWidget(stack_, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(body, 1);
    root->addWidget(error_);
    root->addWidget(buttons);

    reload();
    pages_->setCurrentRow(0);
}

void SettingsDialog::showPage(int page) {
    if (page < 0 || page >= stack_->count()) return;
    pages_->setCurrentRow(page);
}

int SettingsDialog::page() const { return stack_->currentIndex(); }

QString SettingsDialog::error() const { return error_->text(); }

void SettingsDialog::setError(const QString& text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

void SettingsDialog::reload() {
    reloadGeneral();
    reloadProfile();
    reloadEditor();
}

// ---- General ---------------------------------------------------------------------------------------------------------------------

void SettingsDialog::buildGeneral(QWidget* page) {
    auto* v = new QVBoxLayout(page);
    v->addWidget(section(QStringLiteral("Game folder"), page));
    v->addWidget(note(QStringLiteral("The game's data folder (for example GameData): textures, maps, prefabs, entities.ent and game_profile.json are found in it, "
                                     "and the paths in maps are relative to it."),
                      page));
    auto* row = new QHBoxLayout;
    root_ = new QLineEdit(page);
    root_->setObjectName(QStringLiteral("settings:root"));
    auto* browse = new QPushButton(QStringLiteral("Browse..."), page);
    browse->setObjectName(QStringLiteral("settings:browse"));
    QObject::connect(browse, &QPushButton::clicked, this, [this] { browseRoot(); });
    row->addWidget(root_, 1);
    row->addWidget(browse);
    v->addLayout(row);
    rootInfo_ = note({}, page);
    rootInfo_->setObjectName(QStringLiteral("settings:rootInfo"));
    v->addWidget(rootInfo_);
    v->addWidget(section(QStringLiteral("Files"), page));
    files_ = note({}, page);
    files_->setObjectName(QStringLiteral("settings:files"));
    v->addWidget(files_);
    v->addStretch(1);
}

void SettingsDialog::reloadGeneral() {
    root_->setText(qs(win_.assetsRoot()));
    QString info;
    if (win_.assetsRoot().empty()) {
        info = QStringLiteral("No game folder chosen yet.");
    } else {
        const QFileInfo dir(qs(win_.assetsRoot()));
        if (!dir.isDir()) info = QStringLiteral("This folder does not exist.");
        else {
            const core::EntityDefinitions* defs = win_.entityDefinitions();
            info = QStringLiteral("%1 entity classes, prefabs in \"%2\".").arg(defs ? defs->classes.size() : size_t(0)).arg(qs(win_.prefabRoot()));
            if (!win_.profileError().empty()) info = QStringLiteral("game_profile.json is broken: ") + qs(win_.profileError());
        }
    }
    rootInfo_->setText(info);
    QString files;
    if (!win_.assetsRoot().empty()) {
        const QString profile = qs(win_.assetsRoot()) + QStringLiteral("/game_profile.json");
        files += QStringLiteral("Game profile: %1%2\n").arg(profile, QFileInfo::exists(profile) ? QString() : QStringLiteral(" (not there yet: the defaults are used)"));
    }
    if (QSettings* st = win_.settings()) files += QStringLiteral("Editor settings: %1").arg(QDir::toNativeSeparators(st->fileName()));
    else files += QStringLiteral("Editor settings: not saved (no settings file)");
    files_->setText(files);
}

void SettingsDialog::browseRoot() {
    const QString start = root_->text().isEmpty() ? QDir::homePath() : root_->text();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Game folder"), start);
    if (!dir.isEmpty()) root_->setText(dir);
}

QString SettingsDialog::applyGeneral() {
    const QString wanted = cleanFolder(root_->text());
    if (wanted == cleanFolder(qs(win_.assetsRoot()))) return {};
    if (!wanted.isEmpty() && !QFileInfo(wanted).isDir()) return QStringLiteral("Game folder: \"%1\" is not a folder.").arg(wanted);
    if (win_.isDirty() &&
        !win_.confirm(QStringLiteral("Unsaved changes"),
                      QStringLiteral("The open map has unsaved changes. It stays open, but saving will write it into the new game folder. Switch the game folder?")))
        return QStringLiteral("The game folder was not changed.");
    win_.setAssetsRoot(str(wanted));
    return {};
}

// ---- Game profile ----------------------------------------------------------------------------------------------------------------

void SettingsDialog::buildProfile(QWidget* page) {
    profilePage_ = page;
    auto* v = new QVBoxLayout(page);
    profileNote_ = note({}, page);
    profileNote_->setObjectName(QStringLiteral("settings:profileNote"));
    v->addWidget(profileNote_);
    auto* form = new QFormLayout;
    const auto line = [&](QLineEdit*& slot, const char* objectName, const QString& label, const QString& tip = {}) {
        slot = new QLineEdit(page);
        slot->setObjectName(QString::fromLatin1(objectName));
        slot->setToolTip(tip);
        form->addRow(label, slot);
    };
    line(name_, "settings:profile:name", QStringLiteral("Name"));
    exporter_ = new QComboBox(page);
    exporter_->setObjectName(QStringLiteral("settings:profile:exporter"));
    exporter_->addItem(QStringLiteral("rBSP (.bsp)"), QStringLiteral("rbsp"));
    exporter_->addItem(QStringLiteral("OBJ + JSON (.obj)"), QStringLiteral("obj"));
    exporter_->setToolTip(QStringLiteral("What File > Export writes: rBSP for the engine, or OBJ + JSON (.obj geometry, .json entities, light volume, lightmaps)"));
    form->addRow(QStringLiteral("Export format"), exporter_);
    form->addRow(section(QStringLiteral("Folders (inside the game folder)"), page));
    line(textures_, "settings:profile:textureRoot", QStringLiteral("Textures"));
    line(maps_, "settings:profile:mapRoot", QStringLiteral("Maps"));
    line(prefabs_, "settings:profile:prefabRoot", QStringLiteral("Prefabs"));
    line(texGroups_, "settings:profile:texGroupRoot", QStringLiteral("Texture groups"));
    line(entities_, "settings:profile:entityDefinitions", QStringLiteral("Entity definitions"), QStringLiteral("Quake 3 style .ent (XML) file"));
    form->addRow(section(QStringLiteral("Files"), page));
    line(texExt_, "settings:profile:textureExtensions", QStringLiteral("Texture extensions"), QStringLiteral("Looked for in this order, for example .tga, .jpg, .png"));
    line(modelExt_, "settings:profile:modelExtensions", QStringLiteral("Model extensions"));
    ppm_ = new QDoubleSpinBox(page);
    ppm_->setObjectName(QStringLiteral("settings:profile:pixelsPerMeter"));
    ppm_->setRange(0.01, 100000);
    ppm_->setDecimals(2);
    ppm_->setToolTip(QStringLiteral("Texture pixels per metre when a texture has no properties of its own (64 = Quake 3)"));
    form->addRow(QStringLiteral("Texture scale (px/m)"), ppm_);
    form->addRow(section(QStringLiteral("Lighting classes"), page));
    block_ = new QPlainTextEdit(page);
    block_->setObjectName(QStringLiteral("settings:profile:blockLight"));
    block_->setPlaceholderText(QStringLiteral("func_door"));
    block_->setFixedHeight(64);
    receive_ = new QPlainTextEdit(page);
    receive_->setObjectName(QStringLiteral("settings:profile:receiveBakedLight"));
    receive_->setPlaceholderText(QStringLiteral("func_wall"));
    receive_->setFixedHeight(64);
    form->addRow(QStringLiteral("Block light"), block_);
    form->addRow(QStringLiteral("Receive baked light"), receive_);
    v->addLayout(form);
    v->addWidget(note(QStringLiteral("One classname per line. worldspawn, func_group and func_detail always block light and receive baked light."), page));
    v->addStretch(1);
}

tools::ProfileForm SettingsDialog::readProfileForm() const {
    tools::ProfileForm f;
    f.name = str(name_->text().trimmed());
    f.textureRoot = str(textures_->text().trimmed());
    f.mapRoot = str(maps_->text().trimmed());
    f.prefabRoot = str(prefabs_->text().trimmed());
    f.texGroupRoot = str(texGroups_->text().trimmed());
    f.entityDefinitions = str(entities_->text().trimmed());
    f.textureExtensions = str(texExt_->text());
    f.modelExtensions = str(modelExt_->text());
    f.pixelsPerMeter = ppm_->value();
    f.exporter = str(exporter_->currentData().toString());
    f.blockLight = str(block_->toPlainText());
    f.receiveBakedLight = str(receive_->toPlainText());
    return f;
}

void SettingsDialog::reloadProfile() {
    const bool have = !win_.assetsRoot().empty() && QFileInfo(qs(win_.assetsRoot())).isDir();
    profileLoaded_ = false;
    profilePage_->setEnabled(have);
    if (!have) {
        profileNote_->setText(QStringLiteral("Choose the game folder first (General)."));
        return;
    }
    const tools::LoadedForm loaded = tools::loadProfileForm(win_.assetsRoot());
    profileOriginal_ = loaded.form;
    const tools::ProfileForm& f = loaded.form;
    name_->setText(qs(f.name));
    textures_->setText(qs(f.textureRoot));
    maps_->setText(qs(f.mapRoot));
    prefabs_->setText(qs(f.prefabRoot));
    texGroups_->setText(qs(f.texGroupRoot));
    entities_->setText(qs(f.entityDefinitions));
    texExt_->setText(qs(f.textureExtensions));
    modelExt_->setText(qs(f.modelExtensions));
    ppm_->setValue(f.pixelsPerMeter);
    exporter_->setCurrentIndex(std::max(0, exporter_->findData(qs(f.exporter))));
    block_->setPlainText(qs(f.blockLight));
    receive_->setPlainText(qs(f.receiveBakedLight));
    if (!loaded.error.empty())
        profileNote_->setText(QStringLiteral("game_profile.json cannot be read (%1): the built-in defaults are shown, and Apply replaces the file.").arg(qs(loaded.error)));
    else if (!loaded.fileExists)
        profileNote_->setText(QStringLiteral("This folder has no game_profile.json yet: the values are the built-in defaults, and Apply creates the file."));
    else
        profileNote_->setText(QStringLiteral("game_profile.json of the game folder. Keys not shown here are kept as they are."));
    profileLoaded_ = true;
}

QString SettingsDialog::applyProfile() {
    if (!profileLoaded_) return {};
    const tools::ProfileForm edited = readProfileForm();
    if (edited == profileOriginal_) return {};
    const std::string err = tools::saveProfileForm(win_.assetsRoot(), profileOriginal_, edited);
    if (!err.empty()) return QStringLiteral("Game profile: ") + qs(err);
    win_.reloadProfile();
    return {};
}

// ---- Editor ----------------------------------------------------------------------------------------------------------------------

void SettingsDialog::buildEditor(QWidget* page) {
    auto* v = new QVBoxLayout(page);
    v->addWidget(section(QStringLiteral("New viewports"), page));
    v->addWidget(note(QStringLiteral("What a viewport starts with when it is created (a new layout, or a new install). Open viewports keep their own settings."), page));
    auto* form = new QFormLayout;
    grid_ = new QComboBox(page);
    grid_->setObjectName(QStringLiteral("settings:editor:grid"));
    fillNumbers(grid_, render::gridSizes(), QStringLiteral(" m"));
    snap_ = new QCheckBox(QStringLiteral("Snap to grid"), page);
    snap_->setObjectName(QStringLiteral("settings:editor:snap"));
    rot_ = new QComboBox(page);
    rot_->setObjectName(QStringLiteral("settings:editor:rot"));
    fillNumbers(rot_, render::rotSnaps(), QStringLiteral("\xC2\xB0"));
    scale_ = new QComboBox(page);
    scale_->setObjectName(QStringLiteral("settings:editor:scale"));
    fillNumbers(scale_, render::scaleSnaps(), QString());
    fly_ = new QDoubleSpinBox(page);
    fly_->setObjectName(QStringLiteral("settings:editor:fly"));
    fly_->setRange(render::kFlySpeedMin, render::kFlySpeedMax);
    fly_->setDecimals(2);
    showGrid_ = new QCheckBox(QStringLiteral("Show grid"), page);
    showGrid_->setObjectName(QStringLiteral("settings:editor:showGrid"));
    form->addRow(QStringLiteral("Grid size"), grid_);
    form->addRow(QString(), snap_);
    form->addRow(QStringLiteral("Rotation snap"), rot_);
    form->addRow(QStringLiteral("Scale snap"), scale_);
    form->addRow(QStringLiteral("Fly speed (m/s)"), fly_);
    form->addRow(QString(), showGrid_);
    v->addLayout(form);
    auto* use = new QPushButton(QStringLiteral("Use the active viewport's values"), page);
    use->setObjectName(QStringLiteral("settings:editor:useViewport"));
    QObject::connect(use, &QPushButton::clicked, this, [this] { useViewportValues(); });
    v->addWidget(use, 0, Qt::AlignLeft);
    v->addWidget(section(QStringLiteral("Recent maps"), page));
    auto* row = new QHBoxLayout;
    recentInfo_ = new QLabel(page);
    recentInfo_->setObjectName(QStringLiteral("settings:editor:recentInfo"));
    clearRecent_ = new QPushButton(QStringLiteral("Clear recent maps"), page);
    clearRecent_->setObjectName(QStringLiteral("settings:editor:clearRecent"));
    QObject::connect(clearRecent_, &QPushButton::clicked, this, [this] {
        win_.clearRecentFiles();
        reloadEditor();
    });
    row->addWidget(recentInfo_);
    row->addWidget(clearRecent_);
    row->addStretch(1);
    v->addLayout(row);
    v->addStretch(1);
}

tools::EditorPrefs SettingsDialog::readEditorPrefs() const {
    tools::EditorPrefs p;
    p.gridSize = grid_->currentData().toDouble();
    p.snapEnabled = snap_->isChecked();
    p.rotSnap = rot_->currentData().toDouble();
    p.scaleSnap = scale_->currentData().toDouble();
    p.flySpeed = fly_->value();
    p.showGrid = showGrid_->isChecked();
    return tools::sanitize(p);
}

void SettingsDialog::reloadEditor() {
    const tools::EditorPrefs& p = win_.editorPrefs();
    selectNumber(grid_, p.gridSize, QStringLiteral(" m"));
    snap_->setChecked(p.snapEnabled);
    selectNumber(rot_, p.rotSnap, QStringLiteral("\xC2\xB0"));
    selectNumber(scale_, p.scaleSnap, QString());
    fly_->setValue(p.flySpeed);
    showGrid_->setChecked(p.showGrid);
    const size_t n = win_.recentFiles().size();
    recentInfo_->setText(n ? QStringLiteral("%1 map%2 in File > Open Recent").arg(n).arg(n == 1 ? "" : "s") : QStringLiteral("No recent maps"));
    clearRecent_->setEnabled(n > 0);
}

void SettingsDialog::useViewportValues() {
    render::ViewportWindow* vp = win_.activeViewport();
    if (!vp) return;
    const tools::EditorPrefs p = tools::prefsOf(vp->rig().settings());
    selectNumber(grid_, p.gridSize, QStringLiteral(" m"));
    snap_->setChecked(p.snapEnabled);
    selectNumber(rot_, p.rotSnap, QStringLiteral("\xC2\xB0"));
    selectNumber(scale_, p.scaleSnap, QString());
    fly_->setValue(p.flySpeed);
    showGrid_->setChecked(p.showGrid);
}

QString SettingsDialog::applyEditor() {
    const tools::EditorPrefs p = readEditorPrefs();
    if (p != win_.editorPrefs()) win_.setEditorPrefs(p);
    return {};
}

// ---- all -------------------------------------------------------------------------------------------------------------------------

bool SettingsDialog::dirty() const {
    if (cleanFolder(root_->text()) != cleanFolder(qs(win_.assetsRoot()))) return true;
    if (profileLoaded_ && readProfileForm() != profileOriginal_) return true;
    return readEditorPrefs() != win_.editorPrefs();
}

QString SettingsDialog::applyAll() {
    setError({});
    for (const auto apply : {&SettingsDialog::applyEditor, &SettingsDialog::applyProfile, &SettingsDialog::applyGeneral}) {
        const QString err = (this->*apply)();
        if (!err.isEmpty()) {
            setError(err);
            return err;
        }
    }
    reload();
    return {};
}

} // namespace native::ui
