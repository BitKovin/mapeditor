// M4 gate: block out a small level from scratch (boxes, a light, a player start), save it, reopen it. The Shell reads maps through the Core, so "opens in the
// old Shell" is checked as: the saved file is read by core::documentFromJson (the Shell's reader) and passes core::validateDocument, with the same boxes and
// entities; the native editor reads it back to the same version bytes and the same scene. A real window and GPU (native_gui_tests).
#include <doctest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "base/jobs.h"
#include "core/document.h"
#include "doc/commands.h"
#include "doc/io.h"
#include "gui_support.h"
#include "render/picking.h"

using namespace native;
using namespace guitest;
namespace fs = std::filesystem;

namespace {

const char* kGame = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";

QPoint screenOf(render::ViewportWindow* vp, const render::Vec3& p) {
    const render::ViewState view = vp->currentView();
    const render::Vec3 n = render::mat4Transform(view.viewProj, p);
    return QPoint(static_cast<int>((n.x * 0.5 + 0.5) * vp->width()), static_cast<int>((n.y * 0.5 + 0.5) * vp->height()));
}

size_t brushCount(const doc::DocumentVersion& v) {
    size_t n = 0;
    for (const auto& e : v.entities) n += e->brushes.size();
    return n;
}

size_t entityCount(const doc::DocumentVersion& v, const std::string& classname) {
    size_t n = 0;
    for (const auto& e : v.entities) n += e->typed.classname == classname ? 1 : 0;
    return n;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("M4 gate: a level blocked out from scratch is saved and read back as the same map" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    if (!fs::exists(fs::path(kGame) / "entities.ent")) return;   // the game's entity definitions give the light and the player start
    base::JobPool pool(4);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QFile::copy(QString::fromUtf8(kGame) + "/entities.ent", dir.path() + "/entities.ent");
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
    const fs::path file = fs::path(dir.path().toStdString()) / "maps" / "gate.map.json";

    std::string savedBytes;
    size_t brushesBefore = 0, lightsBefore = 0, startsBefore = 0, objectsBefore = 0;
    {
        ui::MainWindow window(sv);
        window.resize(1000, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));

        // File > New: an empty map without a path
        REQUIRE(window.runAction("file.new"));
        REQUIRE(window.document());
        CHECK(window.document()->entities.empty());
        CHECK(window.document()->path.empty());
        CHECK_FALSE(window.isDirty());
        pumpFor(500);   // an empty scene is never "ready" (no objects): give the scene builder a moment

        render::ViewportWindow* vp = window.viewports()[0].window;
        vp->rig().frame({-24, -1, -24}, {24, 8, 24});
        vp->viewChanged();
        pumpFor(300);

        // the box tool on the ground: floor, then two walls (a rectangle, then the height)
        window.setTool(ui::MainWindow::Tool::Box);
        const auto move = [&](QPoint p, Qt::MouseButtons buttons = Qt::NoButton) { sendMouse(vp, QEvent::MouseMove, Qt::NoButton, buttons, p); };
        const auto press = [&](QPoint p) { sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, p); };
        const auto release = [&](QPoint p) { sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, p); };
        const auto drawBox = [&](render::Vec3 a, render::Vec3 b, int heightPixels) {
            const size_t before = brushCount(*window.document());
            const QPoint pa = screenOf(vp, a), pb = screenOf(vp, b);
            move(pa);
            press(pa);
            move(pb, Qt::LeftButton);
            release(pb);
            const QPoint ph = pb + QPoint(0, -heightPixels);
            move(ph);
            press(ph);
            release(ph);
            pumpFor(50);
            CHECK_FALSE(window.drawingBox());
            REQUIRE(brushCount(*window.document()) == before + 1);
        };
        drawBox({-16, 0, -16}, {16, 0, 16}, 14);
        drawBox({-16, 0, -16}, {16, 0, -14}, 90);
        drawBox({-16, 0, -16}, {-14, 0, 16}, 90);
        CHECK(window.isDirty());
        CHECK(brushCount(*window.document()) == 3);
        for (const auto& e : window.document()->entities)
            for (const auto& b : e->brushes) CHECK_FALSE(b->brush.checkConvexSolid());   // every brush is a valid convex solid

        // a light and a player start from the Place panel's path
        REQUIRE(window.placeEntity("light_point"));
        REQUIRE(window.placeEntity("info_player_start"));
        CHECK(entityCount(*window.document(), "light_point") == 1);
        CHECK(entityCount(*window.document(), "info_player_start") == 1);
        CHECK(window.history().undoCount() == 5);

        // Save asks for a path while there is none, Save As writes it
        window.saveDocumentAs(file.generic_string());
        REQUIRE(waitUntil([&] { return !window.busy() && fs::exists(file); }, 10000));
        CHECK_FALSE(window.isDirty());
        CHECK(window.document()->path == "maps/gate.map.json");

        brushesBefore = brushCount(*window.document());
        lightsBefore = entityCount(*window.document(), "light_point");
        startsBefore = entityCount(*window.document(), "info_player_start");
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker(); }, 20000));
        objectsBefore = window.picker()->objectCount();
        CHECK(objectsBefore >= 3);
        savedBytes = readAll(file);
        REQUIRE_FALSE(savedBytes.empty());
    }

    // read by the Shell's reader (the Core): valid, with the same content
    {
        const nlohmann::json j = nlohmann::json::parse(savedBytes);
        const core::Document coreDoc = core::documentFromJson(j);
        CHECK_FALSE(core::validateDocument(coreDoc).has_value());
        size_t brushes = 0, lights = 0, starts = 0;
        for (const core::Entity& e : coreDoc.entities) {
            brushes += e.brushes.size();
            lights += e.classname == "light_point" ? 1 : 0;
            starts += e.classname == "info_player_start" ? 1 : 0;
        }
        CHECK(brushes == brushesBefore);
        CHECK(lights == lightsBefore);
        CHECK(starts == startsBefore);
        CHECK(brushes == 3);
    }

    // reopened in a fresh native window: the same bytes when saved again, the same scene
    {
        ui::MainWindow window(sv);
        window.resize(1000, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.openDocument(file.generic_string());
        REQUIRE(waitUntil([&] { return window.document() && !window.busy(); }, 10000));
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker() && window.picker()->objectCount() == objectsBefore; }, 20000));
        CHECK(doc::serializeDocument(*window.document()) == savedBytes);
        CHECK_FALSE(window.isDirty());
        CHECK(brushCount(*window.document()) == brushesBefore);
    }
}
