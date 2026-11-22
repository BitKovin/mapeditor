// M11 item 11, U7: camera bookmarks (nine per map, in the user settings under the map's path) with a real window and viewports (native_gui_tests).
#include <doctest.h>

#include <QSettings>
#include <QTemporaryDir>

#include "base/jobs.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "gui_support.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> mapAt(const std::string& path) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    auto copy = std::make_shared<doc::DocumentVersion>(*v);
    copy->path = path;
    return copy;
}

} // namespace

TEST_CASE("camera bookmarks: saved per map and slot, recalled with the mode and the pose; they stay in the settings file" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir dir;
    const QString file = dir.filePath("settings.ini");
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
    {
        QSettings settings(file, QSettings::IniFormat);
        sv.settings = &settings;
        ui::MainWindow window(sv);
        window.resize(900, 650);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(mapAt("maps/one.map.json"), true);
        render::ViewportWindow* vp = window.activeViewport();
        REQUIRE(vp);
        CHECK_FALSE(window.hasCameraBookmark(1));
        CHECK_FALSE(window.recallCameraBookmark(1));
        vp->rig().fly().position = {3, 4, 5};
        CHECK(window.saveCameraBookmark(1));
        CHECK(window.hasCameraBookmark(1));
        CHECK_FALSE(window.hasCameraBookmark(2));
        vp->rig().fly().position = {-9, 0, 0};
        vp->setCameraMode(render::CameraMode::Top);
        CHECK(window.recallCameraBookmark(1));
        CHECK(vp->rig().mode() == render::CameraMode::Fly);
        CHECK(vp->rig().fly().position.x == doctest::Approx(3));
        CHECK(vp->rig().fly().position.y == doctest::Approx(4));
        // another map has its own bookmarks
        window.setDocument(mapAt("maps/two.map.json"), true);
        CHECK_FALSE(window.hasCameraBookmark(1));
        window.setDocument(mapAt("maps/one.map.json"), true);
        CHECK(window.hasCameraBookmark(1));
        // slots outside 1..9 are refused; the actions exist (recall off for an empty slot)
        CHECK_FALSE(window.saveCameraBookmark(0));
        CHECK_FALSE(window.saveCameraBookmark(10));
        CHECK(window.runAction("camera.bookmark.save", {{"slot", 4}}));
        CHECK(window.hasCameraBookmark(4));
        vp->rig().fly().position = {7, 8, 9};
        REQUIRE(window.saveCameraBookmark(3));
        settings.sync();
    }
    {   // a later session
        QSettings settings(file, QSettings::IniFormat);
        sv.settings = &settings;
        ui::MainWindow window(sv);
        window.resize(900, 650);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(mapAt("maps/one.map.json"), true);
        REQUIRE(window.activeViewport());
        REQUIRE(window.hasCameraBookmark(3));
        CHECK(window.recallCameraBookmark(3));
        CHECK(window.activeViewport()->rig().fly().position.z == doctest::Approx(9));
        CHECK(window.runAction("camera.bookmark.recall", {{"slot", 1}}));
        CHECK(window.activeViewport()->rig().fly().position.z == doctest::Approx(5));
    }
}
