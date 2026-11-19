// T3.5 (Qt side): click, Ctrl-click, Shift-click and box select in a viewport go through the action system to the picker and the selection, and the
// selection is drawn as an outline (a real window and GPU: native_gui_tests).
#include <doctest.h>

#include <QTemporaryDir>

#include <filesystem>

#include "base/jobs.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "render/entity_overlay.h"
#include "render/picking.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity e;
    e.id = "w";
    e.classname = "worldspawn";
    e.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

// Pixels that look like the selection outline (orange) or the rubber band (white, drawn on top).
int countOrange(const render::CapturedImage& img) {
    int n = 0;
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x) {
            const Rgb p = pixel(img, x, y);
            if (p.r > 200 && p.g > 110 && p.g < 200 && p.b < 90) ++n;
        }
    return n;
}

} // namespace

TEST_CASE("clicks and drags in a viewport select, and the selection is outlined" * doctest::test_suite("gpu")) {
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
        window.setDocument(oneBox(), true);
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker() && window.picker()->objectCount() == 1; }, 20000));
        pumpFor(200);
        render::ViewportWindow* vp = window.viewports()[0].window;
        vp->rig().fly().position = {0, 0, 6};
        vp->rig().fly().yaw = 3.14159265358979;   // towards -Z, at the box
        vp->rig().fly().pitch = 0;
        vp->viewChanged();
        pumpFor(100);
        const QPoint mid(vp->width() / 2, vp->height() / 2);
        const QPoint corner(8, 8);
        const doc::SelRef brush = doc::SelRef::brush("w", "b");
        const auto click = [&](QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, p, mods);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, p, mods);
        };

        const auto before = vp->capture();
        REQUIRE(before.ok());
        CHECK(countOrange(before) == 0);

        click(mid);
        REQUIRE(window.selection().items().size() == 1);
        CHECK(window.selection().items()[0] == brush);
        pumpFor(200);
        const auto outlined = vp->capture();
        REQUIRE(outlined.ok());
        CHECK(countOrange(outlined) > 40);   // the edges of the selected box

        // the size of the selected box: one label per axis (the box is 2 m each way, every number once), none without a selection
        {
            const auto labels = window.sizeLabelsFor(0);
            REQUIRE(!labels.empty());
            CHECK(labels.size() <= 3);
            for (const auto& l : labels) CHECK(l.text == "2 m");
        }
        click(corner);                       // nothing there: clears
        CHECK(window.selection().empty());
        CHECK(window.sizeLabelsFor(0).empty());

        click(mid, Qt::ShiftModifier);       // Shift: the face under the cursor
        REQUIRE(window.selection().items().size() == 1);
        CHECK(window.selection().items()[0].isFace());

        click(mid);
        click(corner, Qt::ControlModifier);  // Ctrl on nothing changes nothing
        CHECK(window.selection().items().size() == 1);
        click(mid, Qt::ControlModifier);     // Ctrl toggles it off
        CHECK(window.selection().empty());
        click(mid, Qt::ControlModifier);     // and on
        CHECK(window.selection().items().size() == 1);

        // a drag over the whole viewport selects what lies inside; the rubber band is drawn meanwhile
        window.selection().clear();
        const QPoint a(4, 4), b(vp->width() - 4, vp->height() - 4);
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, a);
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, QPoint(a.x() + 2, a.y() + 1));
        CHECK(window.selection().empty());   // under 4 px: still a click
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, b);
        pumpFor(200);
        const auto band = vp->capture();
        REQUIRE(band.ok());
        const auto whiteOnRightEdge = [](const render::CapturedImage& img) {
            int n = 0;
            for (uint32_t y = 0; y < img.height; ++y)
                for (uint32_t x = img.width - 8; x < img.width - 1; ++x) {
                    const Rgb p = pixel(img, x, y);
                    if (p.r > 200 && p.g > 200 && p.b > 200) { ++n; break; }
                }
            return n;
        };
        const int white = whiteOnRightEdge(band);
        CHECK(white > static_cast<int>(band.height) / 2);   // the right edge of the rectangle
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, b);
        REQUIRE(window.selection().items().size() == 1);
        CHECK(window.selection().items()[0] == brush);
        pumpFor(100);
        const auto after = vp->capture();
        const int whiteAfter = whiteOnRightEdge(after);
        CHECK(whiteAfter < white / 4);       // the band is gone
        // a rectangle that misses the box clears the selection
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, QPoint(4, 4));
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, QPoint(40, 40));
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, QPoint(40, 40));
        CHECK(window.selection().empty());
    }
    // nothing leaks but the glyph atlas image, which stays in the render thread's texture table for the life of the application
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}
