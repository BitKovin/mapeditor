// T11.3g: the Fast Texture Tool on the main window (target, one-step edits, live drag as one undo step) and its canvas (hit test, drag, align, zoom). Offscreen platform, the
// window is never shown (the pointer handlers take canvas pixels).
#include <doctest.h>

#include <QApplication>
#include <QPushButton>

#include <cmath>
#include <map>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/texturing/ftt.h"
#include "ui/app_actions.h"
#include "ui/ftt_window.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;
namespace tex = native::tools::tex;

namespace {

std::shared_ptr<const doc::DocumentVersion> oneBox() {
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
    return v;
}

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

const core::texplan::TexInfo* fakeInfo(const std::string& path) {
    static std::map<std::string, core::texplan::TexInfo> infos;
    auto it = infos.find(path);
    if (it == infos.end()) {
        core::texplan::TexInfo i;
        i.path = path;
        i.width = 128;
        i.height = 128;
        i.ppm = 64;
        it = infos.emplace(path, i).first;
    }
    return &it->second;
}

struct Fixture {
    ui::MainWindow window;
    Fixture() : window(services()) {
        window.setTextureInfo([](const std::string& p) { return fakeInfo(p); });
        window.setDocument(oneBox(), true);
    }
    const core::Brush* brush() const { return &window.document()->entities[0]->brushes[0]->brush; }
    std::vector<tex::V2> uvs(uint32_t faceId) const {
        for (const core::BrushFace& f : brush()->faces)
            if (f.id == faceId) {
                std::vector<tex::V2> out;
                for (const core::Vec2& u : f.uvs) out.push_back({u.u, u.v});
                return out;
            }
        return {};
    }
    uint32_t firstFace() const { return brush()->faces.front().id; }
};

double distance(const std::vector<tex::V2>& a, const std::vector<tex::V2>& b) {
    double d = 0;
    if (a.size() != b.size()) return 1e9;
    for (size_t i = 0; i < a.size(); ++i) d += std::abs(a[i][0] - b[i][0]) + std::abs(a[i][1] - b[i][1]);
    return d;
}

} // namespace

TEST_CASE("ftt target: the selected faces of one brush; a selected brush gives all its faces; nothing gives none") {
    Fixture f;
    CHECK_FALSE(f.window.fttTarget());
    f.window.selection().select(SelRef::brush("w", "a"));
    auto t = f.window.fttTarget();
    REQUIRE(t);
    CHECK(t->faceIds.size() == 6);
    f.window.selection().select(SelRef::face("w", "a", f.firstFace()));
    t = f.window.fttTarget();
    REQUIRE(t);
    CHECK(t->faceIds == std::vector<uint32_t>{f.firstFace()});
}

TEST_CASE("ftt edits: mirror / rotate are one undo step; a drag is one undo step however many moves; cancel puts everything back") {
    Fixture f;
    const uint32_t face = f.firstFace();
    f.window.selection().select(SelRef::face("w", "a", face));
    CHECK(f.window.textureCommand("reset"));
    const auto base = f.uvs(face);
    REQUIRE(base.size() >= 3);
    // one-step edit
    const auto c = tex::boundsCentre(*tex::polygonsBounds({tex::faceUvPolygon(*f.brush(), f.brush()->faces[0], f.window.textureInfo())}));
    CHECK(f.window.fttApply("Mirror UV", tex::mirrorXformAbout('u', c[0])));
    CHECK(distance(f.uvs(face), base) > 1e-6);
    f.window.history().undo();
    CHECK(distance(f.uvs(face), base) < 1e-9);
    // a drag
    const auto target = *f.window.fttTarget();
    REQUIRE(f.window.fttBeginDrag("Fast texture"));
    CHECK(f.window.fttDragging());
    CHECK_FALSE(f.window.fttBeginDrag("again"));
    for (int i = 1; i <= 5; ++i) CHECK(f.window.fttDragTo(target, tex::translateXform(0.1 * i, 0.05 * i)));
    f.window.fttEndDrag(true);
    CHECK_FALSE(f.window.fttDragging());
    CHECK(distance(f.uvs(face), base) > 1e-6);
    f.window.history().undo();   // the whole drag in one step
    CHECK(distance(f.uvs(face), base) < 1e-9);
    // cancel
    REQUIRE(f.window.fttBeginDrag("Fast texture"));
    f.window.fttDragTo(target, tex::translateXform(0.5, 0.5));
    CHECK(distance(f.uvs(face), base) > 1e-6);
    f.window.fttEndDrag(false);
    CHECK(distance(f.uvs(face), base) < 1e-9);
}

TEST_CASE("ftt unwrap: Follow needs two faces; with a brush selected it re-projects the faces") {
    Fixture f;
    f.window.selection().select(SelRef::face("w", "a", f.firstFace()));
    CHECK(f.window.fttUnwrap(tex::UnwrapMode::Follow) == 0);
    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(f.window.textureCommand("reset"));
    CHECK(f.window.fttUnwrap(tex::UnwrapMode::Ortho) > 0);
    f.window.history().undo();
}

TEST_CASE("ftt canvas: shows the island of the selection, a drag on the body moves it, handles scale, cancel puts it back, Align edge straightens an edge") {
    Fixture f;
    ui::FttWindow w(f.window);   // not shown
    ui::FttCanvas* cv = w.canvas();
    cv->resize(600, 420);
    w.refreshNow(true);
    CHECK(cv->faceCount() == 0);
    f.window.selection().select(SelRef::face("w", "a", f.firstFace()));
    CHECK(f.window.textureCommand("reset"));
    w.refreshNow(true);
    REQUIRE(cv->faceCount() == 1);
    REQUIRE(cv->bounds());
    CHECK(cv->description().contains(QStringLiteral("1 face of a")));
    const auto base = f.uvs(f.firstFace());
    const tex::V2 centre = tex::boundsCentre(*cv->bounds());
    const QPointF p0 = cv->uvToPixel(centre);
    CHECK(cv->handleAt(p0) == tex::Handle::Move);
    CHECK(cv->handleAt(QPointF(-50, -50)) == tex::Handle::None);
    // move
    REQUIRE(cv->press(p0));
    CHECK(cv->dragging());
    cv->move(p0 + QPointF(30, 12), false);
    cv->move(p0 + QPointF(60, 24), false);
    cv->release();
    CHECK_FALSE(cv->dragging());
    CHECK(distance(f.uvs(f.firstFace()), base) > 1e-6);
    f.window.history().undo();
    CHECK(distance(f.uvs(f.firstFace()), base) < 1e-9);
    w.refreshNow(false);
    // a scale handle, then cancel
    const QPointF se = cv->uvToPixel(tex::handlePoint(*cv->bounds(), tex::Handle::SE));
    CHECK(cv->handleAt(se) == tex::Handle::SE);
    REQUIRE(cv->press(se));
    cv->move(se + QPointF(40, 40), false);
    CHECK(distance(f.uvs(f.firstFace()), base) > 1e-6);
    cv->cancelDrag();
    CHECK(distance(f.uvs(f.firstFace()), base) < 1e-9);
    // zoom keeps the point under the pointer
    const tex::V2 before = cv->pixelToUv(QPointF(200, 150));
    cv->zoomAt(QPointF(200, 150), 1.5);
    const tex::V2 after = cv->pixelToUv(QPointF(200, 150));
    CHECK(after[0] == doctest::Approx(before[0]));
    CHECK(after[1] == doctest::Approx(before[1]));
    // the toolbar
    static_cast<QPushButton*>(w.button("mirrorU"))->click();
    CHECK(distance(f.uvs(f.firstFace()), base) > 1e-6);
    f.window.history().undo();
    // Align edge on an already straight edge says so and changes nothing
    cv->setAlign(true);
    w.refreshNow(false);
    const QPointF edge = cv->uvToPixel({centre[0], cv->bounds()->min[1]});
    CHECK(cv->press(edge));
    CHECK(cv->status().contains(QStringLiteral("straight")));
    CHECK(distance(f.uvs(f.firstFace()), base) < 1e-9);
}
