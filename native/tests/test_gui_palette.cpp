// T4.7 (Qt side): the command palette dialog, the status bar hints, the key help sheet and the binding names in tooltips, in a real window without render
// services (native_gui_tests).
#include <doctest.h>

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include "gui_support.h"
#include "app/actions/keymap_edit.h"
#include "ui/command_palette.h"
#include "ui/key_help_overlay.h"
#include "ui/place_panel.h"
#include "ui/status_bar.h"

using namespace native;
using namespace guitest;

namespace {

ui::Services keysOnly(const QString& userFile, QSettings* settings) {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.userKeymapPath = userFile.toStdString();
    sv.settings = settings;
    return sv;
}

void typeKey(QWidget* w, int key) {
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(w, &up);
}

} // namespace

TEST_CASE("the command palette finds an enabled action, shows its keys, runs it and remembers it") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    ui::MainWindow window(keysOnly(dir.filePath("keymaps/default.json"), &settings));
    window.show();
    REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
    REQUIRE(window.tool() == ui::MainWindow::Tool::Select);

    {
        ui::CommandPalette palette(window);
        palette.show();
        CHECK(palette.rowCount() > 8);   // an empty query lists everything that can run
        // every entry can be found again by typing its label
        for (int i = 0; i < palette.rowCount(); ++i) {
            const actions::PaletteEntry e = palette.row(i);
            palette.setQuery(QString::fromStdString(e.label));
            bool found = false;
            for (int j = 0; j < palette.rowCount(); ++j) found = found || (palette.row(j).id == e.id && palette.row(j).params == e.params);
            CHECK_MESSAGE(found, e.label);
        }
        palette.setQuery("tool box");
        REQUIRE(palette.rowCount() >= 1);
        CHECK(palette.row(0).id == "tool.set");
        CHECK(palette.row(0).params == nlohmann::json{{"tool", "box"}});
        CHECK(palette.row(0).label == "Tool: Box");
        CHECK(palette.list()->topLevelItem(0)->text(0) == "Tool: Box");
        CHECK(palette.list()->topLevelItem(0)->text(2) == "Tools");
        // Down / Up move in the list while the search box keeps the keys
        palette.setQuery("tool");
        REQUIRE(palette.rowCount() >= 2);
        CHECK(palette.currentRow() == 0);
        typeKey(palette.searchBox(), Qt::Key_Down);
        CHECK(palette.currentRow() == 1);
        typeKey(palette.searchBox(), Qt::Key_Up);
        CHECK(palette.currentRow() == 0);
        palette.setQuery("settings");
        REQUIRE(palette.rowCount() >= 1);
        CHECK(palette.row(0).id == "file.settings");
        CHECK(palette.row(0).keys == "Ctrl+,");
        CHECK(palette.list()->topLevelItem(0)->text(1) == "Ctrl+,");
        // nothing matches: no choice is possible
        palette.setQuery("qqqqq");
        CHECK(palette.rowCount() == 0);
        CHECK_FALSE(palette.choose());

        if (const QByteArray shots = qgetenv("NATIVE_SCREENSHOT_DIR"); !shots.isEmpty()) {   // a look at it (not a check)
            palette.setQuery("");
            pumpFor(50);
            palette.grab().save(QString::fromLocal8Bit(shots) + "/palette_empty.png");
            palette.setQuery("grid sm");
            pumpFor(50);
            palette.grab().save(QString::fromLocal8Bit(shots) + "/palette_query.png");
        }
        palette.setQuery("tool box");
        typeKey(palette.searchBox(), Qt::Key_Return);   // Enter chooses the current row
        REQUIRE(palette.chosen().has_value());
        CHECK(palette.result() == QDialog::Accepted);
        CHECK(palette.runChosen());
    }
    CHECK(window.tool() == ui::MainWindow::Tool::Box);
    REQUIRE_FALSE(window.paletteRecent().empty());
    CHECK(window.paletteRecent()[0].id == "tool.set");

    // the next time the recently used entry comes first for an empty query
    ui::CommandPalette again(window);
    REQUIRE(again.rowCount() > 0);
    CHECK(again.row(0).id == "tool.set");
    CHECK(again.row(0).params == nlohmann::json{{"tool", "box"}});
    // Esc closes without a choice
    again.show();
    typeKey(again.searchBox(), Qt::Key_Escape);
    CHECK_FALSE(again.chosen().has_value());

    // the palette is reachable by its keys
    CHECK(window.keymap().displayText("view.command_palette") == "F3");
    // and a disabled action is not offered: Undo with nothing to undo
    ui::CommandPalette third(window);
    third.setQuery("undo");
    for (int i = 0; i < third.rowCount(); ++i) CHECK(third.row(i).id != "edit.undo");
    // runAction refuses what is disabled and what does not exist
    CHECK_FALSE(window.runAction("edit.undo"));
    CHECK_FALSE(window.runAction("no.such.action"));
    CHECK(window.runAction("tool.set", nlohmann::json{{"tool", "select"}}));
    CHECK(window.tool() == ui::MainWindow::Tool::Select);
}

TEST_CASE("status bar hints follow the tool and the bindings") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ui::MainWindow window(keysOnly(dir.filePath("keymaps/default.json"), nullptr));
    window.show();
    REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
    ui::StatusFields& status = *window.statusFields();
    status.refreshNow();
    CHECK(status.hintText().contains("LMB drag"));
    CHECK(status.hintText().contains("select"));
    CHECK_FALSE(status.hintText().contains("draw"));

    window.setTool(ui::MainWindow::Tool::Box);
    status.refreshNow();
    CHECK(status.hintText().contains("draw"));
    CHECK(status.hintText().contains("Escape cancel"));
    CHECK(status.hintText().contains("T turn shape"));

    // rebinding Escape (cancel drawing) to F9: the hint changes after the keymap does
    actions::BindingEditor ed(window.registry(), window.keymap());
    const auto cancel = window.keymap().bindingsFor("box.cancel");
    REQUIRE_FALSE(cancel.empty());
    const actions::Binding old = *cancel[0];
    REQUIRE(ed.remove(old));
    REQUIRE(ed.add("box.cancel", "tool:box", "F9", nullptr).ok);
    window.keymapEdited();
    REQUIRE(waitUntil([&] { return status.hintText().contains("F9 cancel"); }, 2000));
    CHECK_FALSE(status.hintText().contains("Escape cancel"));
    // the menu text of an action shows its binding the same way (already so since T1.6)
    CHECK(window.hintText() == status.hintText().toStdString());
}

TEST_CASE("key help: held with F1, from the Help menu, made from the active contexts and updated by a rebinding") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ui::MainWindow window(keysOnly(dir.filePath("keymaps/default.json"), nullptr));
    window.show();
    REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
    CHECK_FALSE(window.keyHelpShown());

    sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_F1);
    REQUIRE(window.keyHelpShown());
    const ui::KeyHelpOverlay* sheet = window.keyHelp();
    REQUIRE(sheet);
    CHECK(sheet->sectionCount() >= 1);
    CHECK(sheet->plainText().contains("Everywhere"));
    CHECK(sheet->plainText().contains("Ctrl+S  Save"));
    CHECK(sheet->plainText().contains("F3 / Ctrl+Shift+P  Command palette..."));
    if (const QByteArray shots = qgetenv("NATIVE_SCREENSHOT_DIR"); !shots.isEmpty()) {
        window.setTool(ui::MainWindow::Tool::Box);
        window.statusFields()->refreshNow();
        window.showKeyHelp(true);
        pumpFor(100);
        window.keyHelp()->grab().save(QString::fromLocal8Bit(shots) + "/keyhelp.png");
        window.grab().save(QString::fromLocal8Bit(shots) + "/hint_window.png");
    }
    sendKey(window.windowHandle(), QEvent::KeyRelease, Qt::Key_F1);
    CHECK_FALSE(window.keyHelpShown());

    // the menu entry toggles it and shows it checked
    REQUIRE(window.runAction("help.keys_toggle"));
    CHECK(window.keyHelpShown());
    // a rebinding while it is shown changes its text
    actions::BindingEditor ed(window.registry(), window.keymap());
    REQUIRE(ed.add("file.save", "global", "Ctrl+Alt+F8", nullptr, actions::BindingEditor::Resolve::KeepBoth).ok);
    window.keymapEdited();
    CHECK(window.keyHelp()->plainText().contains("Ctrl+S / Ctrl+Alt+F8  Save"));
    REQUIRE(window.runAction("help.keys_toggle"));
    CHECK_FALSE(window.keyHelpShown());
}

TEST_CASE("tooltips name the key of the action and follow a rebinding") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ui::MainWindow window(keysOnly(dir.filePath("keymaps/default.json"), nullptr));
    window.show();
    REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
    ui::PlacePanel* place = window.place();
    REQUIRE(place);
    auto* box = place->findChild<QToolButton*>(QStringLiteral("shape:box"));
    auto* cylinder = place->findChild<QToolButton*>(QStringLiteral("shape:cylinder"));
    REQUIRE(box);
    REQUIRE(cylinder);
    CHECK(box->toolTip().contains("(B)"));
    CHECK_FALSE(cylinder->toolTip().startsWith("Cylinder ("));   // unbound: no key named
    actions::BindingEditor ed(window.registry(), window.keymap());
    REQUIRE(ed.add("shape.set", "area:viewport", "C", nlohmann::json{{"shape", "cylinder"}}).ok);
    window.keymapEdited();
    CHECK(cylinder->toolTip().contains("(C)"));
    CHECK(box->toolTip().contains("(B)"));
}
