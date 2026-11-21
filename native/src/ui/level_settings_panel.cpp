// ui/level_settings_panel -- see level_settings_panel.h.
#include "ui/level_settings_panel.h"

#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/main_window.h"
#include "ui/texture_browser_panel.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QLabel* note(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setEnabled(false);   // the dimmed text of a hint
    return l;
}

} // namespace

LevelSettingsPanel::LevelSettingsPanel(MainWindow& window, QWidget* parent) : QWidget(parent), win_(window) {
    setObjectName(QStringLiteral("levelPanel"));
    build();
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { refreshNow(); });
    const auto schedule = [this] { timer_->start(); };
    QObject::connect(&win_, &MainWindow::documentEdited, this, schedule);
    QObject::connect(&win_, &MainWindow::documentChanged, this, schedule);
    timer_->start();
}

QPushButton* LevelSettingsPanel::resetButton(const std::string& key) {
    auto* b = new QPushButton(QStringLiteral("↺"), this);
    b->setObjectName(qs("reset:" + key));
    b->setToolTip(QStringLiteral("Back to the default"));
    b->setFixedWidth(28);
    QObject::connect(b, &QPushButton::clicked, this, [this, key] { reset(key); });
    return b;
}

QWidget* LevelSettingsPanel::numberRow(const native::doc::LevelField& f) {
    auto* row = new QWidget(this);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(qs(f.label), row);
    label->setToolTip(qs(f.hint));
    label->setWordWrap(true);
    auto* edit = new QLineEdit(row);
    edit->setObjectName(qs("field:" + f.key));
    edit->setToolTip(qs(f.hint));
    edit->setFixedWidth(96);
    const std::string key = f.key;
    QObject::connect(edit, &QLineEdit::editingFinished, this, [this, edit, key] {
        if (!filling_) setValue(key, edit->text());
    });
    h->addWidget(label, 1);
    h->addWidget(edit);
    h->addWidget(resetButton(f.key));
    return row;
}

void LevelSettingsPanel::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("body"));
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(6, 6, 6, 6);
    scroll->setWidget(body);
    outer->addWidget(scroll);

    const auto section = [&](const QString& title) {
        auto* l = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), body);
        l->setContentsMargins(0, 8, 0, 0);
        v->addWidget(l);
    };
    for (const native::doc::LevelSection& sec : native::doc::levelSections()) {
        section(qs(sec.title));
        if (!sec.note.empty()) v->addWidget(note(qs(sec.note), body));
        for (const native::doc::LevelField& f : sec.fields) v->addWidget(numberRow(f));
        if (sec.title == "Light") {   // the ambient light: a colour and an intensity, stored as worldspawn `_ambient` "r g b"
            auto* row = new QWidget(body);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* label = new QLabel(QStringLiteral("Ambient light"), row);
            label->setToolTip(QStringLiteral("Light added to every texel (worldspawn _ambient)"));
            ambientColor_ = new QPushButton(row);
            ambientColor_->setObjectName(QStringLiteral("ambientColor"));
            ambientColor_->setToolTip(QStringLiteral("Colour of the ambient light"));
            ambientColor_->setFixedWidth(40);
            ambientIntensity_ = new QLineEdit(row);
            ambientIntensity_->setObjectName(QStringLiteral("ambientIntensity"));
            ambientIntensity_->setToolTip(QStringLiteral("Brightness of the ambient light added to every texel; 0 = none"));
            ambientIntensity_->setFixedWidth(56);
            ambientIntensity_->setPlaceholderText(QStringLiteral("0"));
            QObject::connect(ambientColor_, &QPushButton::clicked, this, [this] {
                const QColor start = QColor::fromRgbF(ambientRgb_[0], ambientRgb_[1], ambientRgb_[2]);
                const QColor c = QColorDialog::getColor(start, this, QStringLiteral("Ambient light colour"));
                if (c.isValid()) setAmbient({c.redF(), c.greenF(), c.blueF()}, ambientIntensity_->text().toDouble());
            });
            QObject::connect(ambientIntensity_, &QLineEdit::editingFinished, this, [this] {
                if (!filling_) setAmbient(ambientRgb_, ambientIntensity_->text().toDouble());
            });
            h->addWidget(label, 1);
            h->addWidget(ambientColor_);
            h->addWidget(ambientIntensity_);
            h->addWidget(resetButton("_ambient"));
            v->addWidget(row);
        }
    }

    // the sky (Auto sky, its margin, the level's skybox)
    section(QStringLiteral("Sky"));
    v->addWidget(note(QStringLiteral("Retro sky: on export the level is enclosed in sky faces, drawn by direction from the skybox; sky faces get no lightmap and block no light."), body));
    {
        auto* row = new QWidget(body);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        skyAuto_ = new QComboBox(row);
        skyAuto_->setObjectName(QStringLiteral("skyAuto"));
        skyAuto_->setToolTip(QStringLiteral("Encloses the level in sky faces on BSP export (the map itself is not changed)"));
        for (const native::doc::AutoSkyOption& o : native::doc::autoSkyOptions()) {
            skyAuto_->addItem(QString::fromLatin1(o.label), static_cast<int>(o.value));
            skyAuto_->setItemData(skyAuto_->count() - 1, QString::fromLatin1(o.title), Qt::ToolTipRole);
        }
        QObject::connect(skyAuto_, &QComboBox::activated, this, [this](int i) {
            if (!filling_) setAutoSky(static_cast<native::doc::AutoSky>(skyAuto_->itemData(i).toInt()));
        });
        h->addWidget(new QLabel(QStringLiteral("Auto sky"), row), 1);
        h->addWidget(skyAuto_);
        h->addWidget(resetButton(native::doc::kSkyAutoKey));
        v->addWidget(row);
    }
    {
        auto* row = new QWidget(body);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        skyMargin_ = new QLineEdit(row);
        skyMargin_->setObjectName(QStringLiteral("skyMargin"));
        skyMargin_->setToolTip(QStringLiteral("Space between the level's geometry and its sky box, in metres"));
        skyMargin_->setFixedWidth(96);
        QObject::connect(skyMargin_, &QLineEdit::editingFinished, this, [this] {
            if (!filling_) setSkyMargin(skyMargin_->text());
        });
        h->addWidget(new QLabel(QStringLiteral("Margin (m)"), row), 1);
        h->addWidget(skyMargin_);
        h->addWidget(resetButton(native::doc::kSkyMarginKey));
        v->addWidget(row);
    }
    {
        auto* row = new QWidget(body);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        skyTexture_ = new QLineEdit(row);
        skyTexture_->setObjectName(QStringLiteral("skyTexture"));
        skyTexture_->setPlaceholderText(QStringLiteral("the first skybox in env/"));
        skyTexture_->setToolTip(QStringLiteral("The level's skybox (env/<name>_cube): the automatic sky and the light of light_sky entities"));
        QObject::connect(skyTexture_, &QLineEdit::editingFinished, this, [this] {
            if (!filling_) setSkyTexture(skyTexture_->text());
        });
        h->addWidget(new QLabel(QStringLiteral("Skybox"), row));
        h->addWidget(skyTexture_, 1);
        h->addWidget(resetButton(native::doc::kSkyTextureKey));
        v->addWidget(row);
    }
    skyWarn_ = note(QStringLiteral("Not a skybox: the engine draws only textures named <name>_cube (images in env/) as sky."), body);
    skyWarn_->setObjectName(QStringLiteral("skyWarn"));
    v->addWidget(skyWarn_);
    v->addWidget(note(QStringLiteral("The skybox also gives light_sky entities their light per direction (colour x brightness x the sky)."), body));
    v->addWidget(note(QStringLiteral("Stored in the map as worldspawn keys (Details shows them too). Light, ambient occlusion and optimisation re-bake the lighting; colour correction only changes how it is shown."), body));
    v->addStretch(1);
}

void LevelSettingsPanel::fillField(QLineEdit* edit, const native::doc::LevelField& f, const core::Entity* world) {
    const auto value = native::doc::levelValue(world, f);
    edit->setPlaceholderText(f.dflt ? qs(native::doc::levelNumber(*f.dflt)) : QStringLiteral("lighting.json"));
    if (!edit->hasFocus()) edit->setText(value ? qs(native::doc::levelNumber(*value)) : QString());
    if (auto* r = findChild<QPushButton*>(qs("reset:" + f.key))) r->setEnabled(value.has_value());
}

void LevelSettingsPanel::refreshNow() {
    const auto version = win_.document();
    setEnabled(version != nullptr);
    const core::Entity* world = version ? native::doc::worldspawnOf(*version) : nullptr;
    filling_ = true;
    for (const native::doc::LevelSection& sec : native::doc::levelSections())
        for (const native::doc::LevelField& f : sec.fields)
            if (auto* e = findChild<QLineEdit*>(qs("field:" + f.key))) fillField(e, f, world);
    const native::doc::Ambient amb = native::doc::ambientOf(world);
    if (amb.intensity > 0) ambientRgb_ = amb.color;
    ambientColor_->setStyleSheet(QStringLiteral("background-color: %1").arg(qs(native::doc::hexOfColor(ambientRgb_))));
    if (!ambientIntensity_->hasFocus()) ambientIntensity_->setText(amb.intensity > 0 ? qs(native::doc::levelNumber(amb.intensity)) : QString());
    if (auto* r = findChild<QPushButton*>(QStringLiteral("reset:_ambient"))) r->setEnabled(amb.intensity > 0);

    const native::doc::AutoSky mode = native::doc::autoSkyOf(world);
    skyAuto_->setCurrentIndex(skyAuto_->findData(static_cast<int>(mode)));
    const auto hasKey = [&](const char* key) { return world && world->keyvalues.count(key) != 0; };
    if (!skyMargin_->hasFocus()) skyMargin_->setText(hasKey(native::doc::kSkyMarginKey) ? qs(native::doc::levelNumber(native::doc::skyMarginOf(world))) : QString());
    skyMargin_->setPlaceholderText(qs(native::doc::levelNumber(native::doc::kSkyMarginDefault)));
    const std::string tex = hasKey(native::doc::kSkyTextureKey) ? world->keyvalues.at(native::doc::kSkyTextureKey) : std::string();
    if (!skyTexture_->hasFocus()) skyTexture_->setText(qs(tex));
    skyWarn_->setVisible(!tex.empty() && !native::doc::isCubemapPath(tex));
    if (auto* r = findChild<QPushButton*>(qs(std::string("reset:") + native::doc::kSkyAutoKey))) r->setEnabled(mode != native::doc::AutoSky::Off);
    if (auto* r = findChild<QPushButton*>(qs(std::string("reset:") + native::doc::kSkyMarginKey))) r->setEnabled(hasKey(native::doc::kSkyMarginKey));
    if (auto* r = findChild<QPushButton*>(qs(std::string("reset:") + native::doc::kSkyTextureKey))) r->setEnabled(!tex.empty());
    filling_ = false;
}

QWidget* LevelSettingsPanel::part(const QString& name) const { return findChild<QWidget*>(name); }

bool LevelSettingsPanel::setValue(const std::string& key, const QString& text) {
    const native::doc::LevelField* f = native::doc::levelField(key);
    if (!f) return false;
    const auto stored = native::doc::levelKeyFor(*f, text.toStdString());
    return win_.setLevelKeys(stored ? "Level: " + key : "Level: reset " + key, {{key, stored}});
}

bool LevelSettingsPanel::reset(const std::string& key) { return win_.setLevelKeys("Level: reset " + key, {{key, std::nullopt}}); }

bool LevelSettingsPanel::setAmbient(const std::array<double, 3>& color, double intensity) {
    ambientRgb_ = color;
    const auto stored = native::doc::ambientKey(color, intensity);
    return win_.setLevelKeys(stored ? "Level: _ambient" : "Level: reset _ambient", {{"_ambient", stored}});
}

bool LevelSettingsPanel::setAutoSky(native::doc::AutoSky mode) {
    if (mode == native::doc::AutoSky::Off) return win_.setLevelKeys("Level: auto sky", {{native::doc::kSkyAutoKey, std::nullopt}});
    std::string text;
    for (const native::doc::AutoSkyOption& o : native::doc::autoSkyOptions())
        if (o.value == mode) text = o.text;
    const auto version = win_.document();
    const core::Entity* world = version ? native::doc::worldspawnOf(*version) : nullptr;
    std::string tex;
    if (world) {
        const auto it = world->keyvalues.find(native::doc::kSkyTextureKey);
        if (it != world->keyvalues.end()) tex = it->second;
    }
    if (tex.empty() && win_.textureBrowser()) {   // without a skybox the first one of env/ is taken, so the views can show it
        std::vector<std::string> boxes;
        for (const std::string& t : win_.textureBrowser()->textures())
            if (native::doc::isEnvSkybox(t)) boxes.push_back(t);
        if (!boxes.empty()) tex = *std::min_element(boxes.begin(), boxes.end());
    }
    std::vector<native::doc::LevelChange> changes = {{native::doc::kSkyAutoKey, text}};
    if (!tex.empty()) changes.push_back({native::doc::kSkyTextureKey, tex});
    return win_.setLevelKeys("Level: auto sky", changes);
}

bool LevelSettingsPanel::setSkyMargin(const QString& text) {
    const auto stored = native::doc::skyMarginKey(text.toStdString());
    return win_.setLevelKeys(stored ? std::string("Level: ") + native::doc::kSkyMarginKey : std::string("Level: reset ") + native::doc::kSkyMarginKey, {{native::doc::kSkyMarginKey, stored}});
}

bool LevelSettingsPanel::setSkyTexture(const QString& text) {
    const std::string t = text.trimmed().toStdString();
    return win_.setLevelKeys(std::string("Level: ") + native::doc::kSkyTextureKey, {{native::doc::kSkyTextureKey, t.empty() ? std::nullopt : std::optional<std::string>(t)}});
}

} // namespace native::ui
