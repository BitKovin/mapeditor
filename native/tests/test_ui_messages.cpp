// M11 item 11, U14: the Messages dock keeps what the status bar said.
#include <doctest.h>

#include "ui/main_window.h"
#include "ui/messages_panel.h"

#include <QDateTime>
#include <QStatusBar>

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

} // namespace

TEST_CASE("messages: the status bar's messages are kept with the time, repeats and live readings are merged, the list is limited") {
    ui::MainWindow window(services());
    ui::MessagesPanel* p = window.messagesPanel();
    REQUIRE(p);
    const QDateTime t0(QDate(2026, 11, 22), QTime(12, 3, 41));
    p->add("", t0);
    CHECK(p->rows().isEmpty());
    p->add("Selected 3 items", t0);
    p->add("Selected 3 items", t0.addSecs(1));   // the same text again: one entry
    p->add("Undo Move", t0.addSecs(2));
    CHECK(p->rows() == QStringList{"12:03:41  Selected 3 items", "12:03:43  Undo Move"});
    // a message right after the previous one replaces it (a reading that follows the mouse)
    p->add("Measure: 1.00 m", t0.addSecs(10));
    p->add("Measure: 1.25 m", t0.addSecs(10).addMSecs(50));
    p->add("Measure: 1.50 m", t0.addSecs(10).addMSecs(100));
    CHECK(p->rows().size() == 3);
    CHECK(p->rows().last() == "12:03:51  Measure: 1.50 m");
    // ... but a different message right after one is kept
    p->add("Nudged 0.25 m", t0.addSecs(10).addMSecs(150));
    CHECK(p->rows().size() == 4);
    p->add("Flip X", t0.addSecs(10).addMSecs(160));
    CHECK(p->rows().size() == 5);
    // the real status bar feeds it
    window.statusBar()->showMessage(QStringLiteral("Copied"), 0);
    CHECK(p->rows().last().endsWith("  Copied"));
    CHECK(p->rows().size() == 6);
    // old ones go
    for (int i = 0; i < 300; ++i) p->add(QString("m%1").arg(i), t0.addSecs(100 + i));
    CHECK(p->rows().size() == ui::MessagesPanel::kMaxMessages);
    CHECK(p->rows().last().endsWith("m299"));
    p->clear();
    CHECK(p->rows().isEmpty());
    p->add("Copied", t0.addSecs(500));   // the text of an earlier message is accepted again after Clear
    CHECK(p->rows().size() == 1);
}

TEST_CASE("messages: the dock has a View menu toggle") {
    ui::MainWindow window(services());
    CHECK(window.registry().find("view.messages.toggle") != nullptr);
    CHECK(window.messagesDock() != nullptr);
}
