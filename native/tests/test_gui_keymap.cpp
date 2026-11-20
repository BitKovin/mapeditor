// T4.6 (Qt side): the binding editor in Settings, the recorder and the keymap files, in a real window without render services (native_gui_tests).
#include <doctest.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QWheelEvent>

#include "gui_support.h"
#include "ui/chord_capture.h"
#include "ui/keymap_page.h"
#include "ui/registry_menus.h"
#include "ui/settings_dialog.h"

using namespace native;
using namespace guitest;
using actions::BindingEditor;

namespace {

ui::Services keysOnly(const QString& userFile, QSettings* settings = nullptr) {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.userKeymapPath = userFile.toStdString();
    sv.settings = settings;
    return sv;
}

actions::Binding bind(const std::string& action, const std::string& context, const std::string& input, const nlohmann::json& params = nullptr) {
    actions::Binding b;
    std::string err;
    REQUIRE_MESSAGE(actions::makeBinding(action, context, input, params, false, b, &err), err);
    return b;
}

template <typename W>
W* named(QWidget& root, const char* name) {
    W* w = root.findChild<W*>(QString::fromLatin1(name));
    REQUIRE_MESSAGE(w, name);
    return w;
}

void press(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(w, &up);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

} // namespace

TEST_CASE("the binding editor in Settings: rebinding applies at once, is kept, and can be undone") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString userFile = dir.filePath("keymaps/default.json");
    {
        ui::MainWindow window(keysOnly(userFile));
        window.show();
        REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
        ui::SettingsDialog dialog(window);
        dialog.show();
        dialog.showPage(ui::SettingsDialog::Keymap);
        ui::KeymapPage* page = dialog.keymapPage();
        REQUIRE(page);
        auto* table = named<QTreeWidget>(*page, "keymap:table");

        // every registered action is in the table
        CHECK(static_cast<size_t>(table->topLevelItemCount()) == window.registry().size());
        CHECK(page->shownActions().size() == window.registry().size());

        SUBCASE("search and filters") {
            auto* search = named<QLineEdit>(*page, "keymap:search");
            search->setText("grid");
            const auto shown = page->shownActions();
            CHECK(std::find(shown.begin(), shown.end(), "grid.smaller") != shown.end());
            CHECK(std::find(shown.begin(), shown.end(), "file.save") == shown.end());
            search->setText("ctrl+q");   // words may be in the bindings
            REQUIRE(page->shownActions().size() == 1);
            CHECK(page->shownActions()[0] == "file.exit");
            search->clear();

            auto* filter = named<QComboBox>(*page, "keymap:filter");
            filter->setCurrentIndex(2);   // unbound
            auto unbound = page->shownActions();
            CHECK(std::find(unbound.begin(), unbound.end(), "grid.set") != unbound.end());
            CHECK(std::find(unbound.begin(), unbound.end(), "file.save") == unbound.end());
            filter->setCurrentIndex(3);   // conflicts: none in the default preset
            CHECK(page->shownActions().empty());
            filter->setCurrentIndex(1);   // changed: none yet
            CHECK(page->shownActions().empty());
            filter->setCurrentIndex(0);

            auto* category = named<QComboBox>(*page, "keymap:category");
            category->setCurrentIndex(category->findData("Camera"));
            const auto camera = page->shownActions();
            REQUIRE_FALSE(camera.empty());
            for (const std::string& id : camera) CHECK(id.rfind("camera.", 0) == 0);
            category->setCurrentIndex(0);
            auto* context = named<QComboBox>(*page, "keymap:context");
            context->setCurrentIndex(context->findData("modal:transform"));
            const auto modal = page->shownActions();
            CHECK(std::find(modal.begin(), modal.end(), "transform.confirm") != modal.end());
            context->setCurrentIndex(0);
        }

        SUBCASE("a new binding applies without a restart, shows in the menu and the file") {
            // Exit moves from Ctrl+Q to Ctrl+Alt+F9
            REQUIRE(page->addBinding(bind("file.exit", "global", "Ctrl+Alt+F9")).ok);
            REQUIRE(page->removeBinding(bind("file.exit", "global", "Ctrl+Q")));
            CHECK(window.menus()->entry("file.exit")->text() == QStringLiteral("Exit\tCtrl+Alt+F9"));
            page->selectAction("file.exit");
            CHECK(named<QListWidget>(*page, "keymap:bindings")->count() == 1);
            CHECK(named<QListWidget>(*page, "keymap:bindings")->item(0)->text().contains("[yours]"));
            // the "changed" filter finds it
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(1);
            REQUIRE(page->shownActions().size() == 1);
            CHECK(page->shownActions()[0] == "file.exit");
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(0);
            // the file was written
            const QByteArray written = readFile(userFile);
            CHECK(written.contains("Ctrl+Alt+F9"));

            // the keys act: the old one does nothing, the new one closes the window (the router read the same keymap)
            sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier);
            CHECK(window.isVisible());

            // undo and redo through the buttons
            auto* undo = named<QPushButton>(*page, "keymap:undo");
            auto* redo = named<QPushButton>(*page, "keymap:redo");
            REQUIRE(undo->isEnabled());
            undo->click();
            CHECK(window.keymap().bindingsFor("file.exit").size() == 2);   // the removal of Ctrl+Q is undone: both again
            undo->click();
            CHECK(window.keymap().displayText("file.exit") == "Ctrl+Q");
            CHECK(window.keymap().userEmpty());
            CHECK(window.menus()->entry("file.exit")->text() == QStringLiteral("Exit\tCtrl+Q"));
            redo->click();
            redo->click();
            CHECK(window.keymap().displayText("file.exit") == "Ctrl+Alt+F9");
            sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_F9, Qt::ControlModifier | Qt::AltModifier);
            CHECK_FALSE(window.isVisible());
        }

        SUBCASE("conflicts: cancel, replace, keep both") {
            std::vector<actions::Conflict> seen;
            BindingEditor::Resolve answer = BindingEditor::Resolve::Cancel;
            page->setConflictHandler([&](const std::vector<actions::Conflict>& c) {
                seen = c;
                return answer;
            });
            // Ctrl+O is Open
            auto r = page->addBinding(bind("file.save", "global", "Ctrl+O"));
            CHECK_FALSE(r.ok);
            REQUIRE(seen.size() == 1);
            CHECK(seen[0].actionLabel == "Open...");
            CHECK(window.keymap().userEmpty());

            // Keep both is refused where the preset's binding would be replaced anyway (the same input in the same context) ...
            answer = BindingEditor::Resolve::KeepBoth;
            CHECK_FALSE(page->addBinding(bind("file.save", "global", "Ctrl+O")).ok);
            CHECK(window.keymap().userEmpty());
            // ... and works in a more specific context: the viewport's Ctrl+O wins there, Open keeps its key elsewhere
            CHECK(page->addBinding(bind("file.save", "area:viewport", "Ctrl+O")).ok);
            CHECK(seen.size() == 1);
            CHECK(seen[0].kind == actions::Conflict::Kind::Shadows);
            CHECK(window.keymap().bindingsFor("file.open").size() == 1);
            CHECK(actions::lookupInput(window.registry(), window.keymap(), "Ctrl+O").size() == 2);
            named<QPushButton>(*page, "keymap:undo")->click();
            // two bindings of the user in one context do show as a conflict
            CHECK(page->addBinding(bind("file.save", "global", "F2")).ok);
            CHECK(page->addBinding(bind("file.open", "global", "F2")).ok);
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(3);
            CHECK(page->shownActions().size() == 2);
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(0);
            named<QPushButton>(*page, "keymap:resetAll")->click();

            answer = BindingEditor::Resolve::Replace;
            CHECK(page->addBinding(bind("file.save", "global", "Ctrl+O")).ok);
            CHECK(window.keymap().bindingsFor("file.open").empty());
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(3);
            CHECK(page->shownActions().empty());
            named<QComboBox>(*page, "keymap:filter")->setCurrentIndex(0);
            // reset one action gives Open its key back (and takes Save's extra one away)
            page->selectAction("file.open");
            named<QPushButton>(*page, "keymap:resetAction")->click();
            CHECK(window.keymap().bindingsFor("file.open").size() == 1);
            CHECK(window.keymap().bindingsFor("file.save").size() == 1);
            CHECK(window.keymap().userEmpty());
        }

        SUBCASE("reset all, reverse lookup") {
            REQUIRE(page->addBinding(bind("file.save", "global", "Ctrl+Alt+F1")).ok);
            auto* resetAll = named<QPushButton>(*page, "keymap:resetAll");
            REQUIRE(resetAll->isEnabled());
            resetAll->click();
            CHECK(window.keymap().userEmpty());
            CHECK_FALSE(resetAll->isEnabled());

            auto* lookup = named<ui::ChordCapture>(*page, "keymap:lookup");
            auto* result = named<QLabel>(*page, "keymap:lookupResult");
            lookup->setFocus();
            press(lookup, Qt::Key_Z, Qt::ControlModifier);
            CHECK(lookup->text() == "Ctrl+Z");
            CHECK(result->text().contains("Undo"));
            press(lookup, Qt::Key_F11, Qt::ControlModifier | Qt::AltModifier);
            CHECK(result->text().startsWith("Nothing is bound"));
            // the context is told
            press(lookup, Qt::Key_X);
            CHECK(result->text().contains("modal:transform"));
        }

        SUBCASE("user keymaps: new, switch, import, export, delete") {
            CHECK(window.keymapName() == "default");
            auto* active = named<QComboBox>(*page, "keymap:active");
            CHECK(active->isEnabled());
            REQUIRE(page->addBinding(bind("file.save", "global", "Ctrl+Alt+F2")).ok);
            // a second keymap starts empty and takes over
            CHECK(page->createKeymap("mine").empty());
            CHECK(window.keymapName() == "mine");
            CHECK(window.keymap().userEmpty());
            CHECK(window.keymap().bindingsFor("file.save").size() == 1);
            REQUIRE(page->addBinding(bind("file.save", "global", "Ctrl+Alt+F3")).ok);
            CHECK(active->count() == 2);
            // the names are checked, a taken one is refused
            CHECK_FALSE(page->createKeymap("mine").empty());
            CHECK_FALSE(page->createKeymap("../up").empty());
            // switching saves the current one and loads the other
            CHECK(window.switchKeymap("default").empty());
            CHECK(window.keymap().bindingsFor("file.save").size() == 2);
            CHECK(window.keymap().displayText("file.save") == "Ctrl+S");
            bool hasF2 = false, hasF3 = false;
            for (const auto* b : window.keymap().bindingsFor("file.save")) {
                hasF2 = hasF2 || b->input == "Ctrl+Alt+F2";
                hasF3 = hasF3 || b->input == "Ctrl+Alt+F3";
            }
            CHECK(hasF2);
            CHECK_FALSE(hasF3);
            // export, then import under a new name
            const QString exported = dir.filePath("share/team.json");
            CHECK(page->exportKeymap(exported.toStdString()).empty());
            CHECK(page->importKeymap(exported.toStdString(), "team").empty());
            CHECK(window.keymapName() == "team");
            CHECK(window.keymap().bindingsFor("file.save").size() == 2);   // the same effective keymap as the exported one
            CHECK(readFile(dir.filePath("keymaps/team.json")) == readFile(exported));
            // a file that is not a keymap is refused and nothing changes
            QFile bad(dir.filePath("bad.json"));
            REQUIRE(bad.open(QIODevice::WriteOnly));
            bad.write("{ nope");
            bad.close();
            CHECK_FALSE(page->importKeymap(dir.filePath("bad.json").toStdString(), "bad").empty());
            CHECK(window.keymapName() == "team");
            // delete the active one: back to default
            CHECK(page->removeKeymap("team").empty());
            CHECK(window.keymapName() == "default");
            CHECK_FALSE(QFileInfo::exists(dir.filePath("keymaps/team.json")));
            CHECK(QFileInfo::exists(dir.filePath("keymaps/mine.json")));
        }
    }
    // a clean window on the same folder reads the same effective keymap
    {
        ui::MainWindow again(keysOnly(userFile));
        CHECK(again.keymapError().empty());
        CHECK(again.keymapName() == "default");
    }
}

TEST_CASE("the active user keymap is remembered in the settings") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString userFile = dir.filePath("keymaps/default.json");
    const QString ini = dir.filePath("settings.ini");
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::MainWindow window(keysOnly(userFile, &settings));
        REQUIRE(window.newKeymap("blendish").empty());
        REQUIRE(window.keymap().bindingsFor("file.exit").size() == 1);
        actions::Binding b = bind("file.exit", "global", "Ctrl+Alt+F8");
        window.keymap().bindUser(b);
        window.keymapEdited();
        settings.sync();
    }
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::MainWindow window(keysOnly(userFile, &settings));
        CHECK(window.keymapName() == "blendish");
        CHECK(window.keymap().bindingsFor("file.exit").size() == 2);
        // switching away is remembered too
        REQUIRE(window.switchKeymap("default").empty());
    }
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::MainWindow window(keysOnly(userFile, &settings));
        CHECK(window.keymapName() == "default");
        CHECK(window.keymap().bindingsFor("file.exit").size() == 1);
    }
}

TEST_CASE("the recorder records keys, clicks, drags and the wheel, and says what the input clashes with") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ui::MainWindow window(keysOnly(dir.filePath("keymaps/default.json")));
    window.show();
    const actions::ActionDef* def = window.registry().find("edit.delete");
    REQUIRE(def);
    ui::BindingRecorder rec(window, *def);
    rec.show();
    ui::ChordCapture* cap = rec.capture();
    auto* mode = named<QComboBox>(rec, "keymap:rec:mode");
    auto* status = named<QLabel>(rec, "keymap:rec:status");
    auto* ok = named<QPushButton>(rec, "keymap:rec:ok");
    auto* context = named<QComboBox>(rec, "keymap:rec:context");

    // nothing recorded yet
    CHECK_FALSE(ok->isEnabled());
    CHECK(context->currentData().toString() == "area:viewport");   // Delete's own context

    cap->setFocus();
    press(cap, Qt::Key_F9, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(cap->text() == "Ctrl+Shift+F9");
    CHECK(ok->isEnabled());
    CHECK(status->text().startsWith("Free"));
    const auto e = rec.entry();
    CHECK(e.error.empty());
    CHECK(e.binding.action == "edit.delete");
    CHECK(e.binding.context == "area:viewport");

    // a key that is taken says by what
    press(cap, Qt::Key_G, Qt::ControlModifier);
    CHECK(status->text().contains("Already used by"));
    CHECK(status->text().contains("Group"));

    // the mode applies to keys
    mode->setCurrentIndex(mode->findText("Hold"));
    press(cap, Qt::Key_J);
    CHECK(cap->text() == "Hold J");
    mode->setCurrentIndex(0);

    // a modifier on its own is recorded when it is released alone
    {
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QApplication::sendEvent(cap, &down);
        CHECK(cap->text() == "Hold J");
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QApplication::sendEvent(cap, &up);
        CHECK(cap->text() == "Shift");
        // ... but not when another key came in between
        QKeyEvent down2(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QApplication::sendEvent(cap, &down2);
        press(cap, Qt::Key_K, Qt::ControlModifier);
        QKeyEvent up2(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QApplication::sendEvent(cap, &up2);
        CHECK(cap->text() == "Ctrl+K");
    }

    // the Windows key belongs to the system
    press(cap, Qt::Key_F2, Qt::MetaModifier);
    CHECK_FALSE(rec.entry().error.empty());
    CHECK_FALSE(ok->isEnabled());

    // the wheel
    {
        QWheelEvent w(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(cap, &w);
        CHECK(cap->text() == "Ctrl+Wheel Up");
    }
    // a click, then a drag
    {
        const auto click = [&](Qt::KeyboardModifiers mods, QPoint moveTo) {
            QMouseEvent down(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, mods);
            QApplication::sendEvent(cap, &down);
            if (moveTo != QPoint(5, 5)) {
                QMouseEvent move(QEvent::MouseMove, QPointF(moveTo), QPointF(moveTo), Qt::NoButton, Qt::LeftButton, mods);
                QApplication::sendEvent(cap, &move);
            }
            QMouseEvent up(QEvent::MouseButtonRelease, QPointF(moveTo), QPointF(moveTo), Qt::LeftButton, Qt::NoButton, mods);
            QApplication::sendEvent(cap, &up);
        };
        click(Qt::ShiftModifier, QPoint(5, 5));
        CHECK(cap->text() == "Shift+LMB");
        click(Qt::NoModifier, QPoint(40, 30));
        CHECK(cap->text() == "LMB drag");
    }
    // typing the input instead (a sequence)
    named<QCheckBox>(rec, "keymap:rec:type")->setChecked(true);
    cap->setText("Ctrl+K, Ctrl+S");
    CHECK(rec.entry().error.empty());
    cap->setText("Ctrl+Banana");
    CHECK_FALSE(rec.entry().error.empty());
    CHECK_FALSE(ok->isEnabled());
    // parameters
    cap->setText("F7");
    named<QLineEdit>(rec, "keymap:rec:params")->setText("{ nope");
    CHECK_FALSE(rec.entry().error.empty());
    named<QLineEdit>(rec, "keymap:rec:params")->setText("{\"size\": 0.25}");
    CHECK(rec.entry().binding.params["size"] == 0.25);
}
