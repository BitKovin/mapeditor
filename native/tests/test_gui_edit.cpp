// T4.3 (Qt side): delete, duplicate, group, copy / cut / paste through the OS clipboard, and hiding in a real window (native_gui_tests).
#include <doctest.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QTemporaryDir>

#include "base/jobs.h"
#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "render/picking.h"
#include "tools/visibility.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> twoVolumes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity t;
    t.id = "t";
    t.classname = "trigger_hurt";
    t.brushes.push_back(core::Brush::makeBox("tb", {3, -1, -1}, {5, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(t));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

size_t brushCount(ui::MainWindow& w) {
    size_t n = 0;
    for (const auto& e : w.document()->entities) n += e->brushes.size();
    return n;
}

} // namespace

TEST_CASE("delete, duplicate, group, clipboard and hide act on the selection as undo steps" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir dir;
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
    {
        ui::MainWindow window(sv);
        window.resize(900, 650);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(twoVolumes(), true);
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker() && window.picker()->objectCount() == 2; }, 20000));
        REQUIRE(window.frameScene());
        pumpFor(200);
        const auto sel = [&](const doc::SelRef& s) { window.selection().select(s); };

        SUBCASE("delete and undo") {
            sel(doc::SelRef::brush("w", "b"));
            REQUIRE(window.deleteSelected());
            CHECK(brushCount(window) == 1);
            CHECK(window.selection().empty());
            CHECK(window.history().undoCount() == 1);
            window.undo();
            CHECK(brushCount(window) == 2);
        }

        SUBCASE("duplicate selects the copy; one undo step") {
            sel(doc::SelRef::brush("w", "b"));
            REQUIRE(window.duplicateSelected());
            CHECK(brushCount(window) == 3);
            REQUIRE(window.selection().items().size() == 1);
            CHECK(window.selection().items()[0].brushId != "b");
            CHECK(window.history().undoCount() == 1);
        }

        SUBCASE("group and ungroup") {
            sel(doc::SelRef::brush("w", "b"));
            REQUIRE(window.groupSelected());
            CHECK(window.document()->entities.size() == 3);
            CHECK(window.document()->entities.back()->typed.classname == "func_group");
            REQUIRE(window.selection().items().size() == 1);
            REQUIRE(window.ungroupSelected());
            CHECK(window.document()->entities[0]->brushes.size() == 1);
            CHECK(window.history().undoCount() == 2);
        }

        SUBCASE("copy, paste, cut through the OS clipboard") {
            sel(doc::SelRef::brush("w", "b"));
            REQUIRE(window.copySelected());
            const QString text = QGuiApplication::clipboard()->text();
            CHECK(text.contains("level-editor/clipboard"));
            REQUIRE(window.pasteClipboard({4, 0, 0}));
            CHECK(brushCount(window) == 3);
            CHECK(window.history().undoCount() == 1);
            window.undo();
            CHECK(brushCount(window) == 2);
            sel(doc::SelRef::brush("w", "b"));
            REQUIRE(window.cutSelected());
            CHECK(brushCount(window) == 1);
            QGuiApplication::clipboard()->setText("just text");
            CHECK_FALSE(window.pasteClipboard());
        }

        SUBCASE("hidden things are not drawn, picked or selected; no undo step") {
            REQUIRE(window.picker()->pick(render::Ray{{4, 0, 10}, {0, 0, -1}}));
            sel(doc::SelRef::brush("t", "tb"));
            REQUIRE(window.hideSelected());
            CHECK(window.selection().empty());
            CHECK(window.history().undoCount() == 0);
            REQUIRE(window.hiddenObjects());
            CHECK(window.hiddenObjects()->size() >= 1);
            const render::PickOptions o = [&] { render::PickOptions x; x.hidden = window.hiddenObjects().get(); return x; }();
            CHECK_FALSE(window.picker()->pick(render::Ray{{4, 0, 10}, {0, 0, -1}}, o));
            CHECK(window.picker()->pick(render::Ray{{0, 0, 10}, {0, 0, -1}}, o));
            window.unhideAll();
            CHECK_FALSE(window.hiddenObjects());
            // by class rule
            std::vector<tools::HideRule> rules = window.hideState().rules;
            rules = tools::addRule(rules, "trigger_*");
            window.setHideRules(rules);
            REQUIRE(window.hiddenObjects());
            sel(doc::SelRef::brush("w", "b"));
            CHECK(window.selection().items().size() == 1);
            window.setHideRules(tools::defaultHideRules());
            CHECK_FALSE(window.hiddenObjects());
        }
    }
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}
