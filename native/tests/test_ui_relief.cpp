// T11.5b: the relief editor window (offscreen, never shown): the projection that draws also picks, selection by click and box, the modal tools as one undo step, presets, depth, the picture
// on the faces, and saving through a fake sidecar store (no assets folder).
#include <doctest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>

#include <map>

#include "ui/app_actions.h"
#include "ui/main_window.h"
#include "ui/relief_editor.h"
#include "ui/texture_props_window.h"

using namespace native;
namespace rl = native::tools::relief;
namespace mesh = native::tools::mesh;
using json = nlohmann::json;
namespace tp = core::texplan;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

const tp::TexInfo* fakeInfo(const std::string& path) {
    static std::map<std::string, tp::TexInfo> infos;
    auto it = infos.find(path);
    if (it == infos.end()) {
        tp::TexInfo i;
        i.path = path;
        i.width = 128;   // 2 x 1 m at 64 pixels per metre
        i.height = 64;
        i.ppm = 64;
        i.props = tp::defaultProps();
        it = infos.emplace(path, i).first;
    }
    return &it->second;
}

struct Fixture {
    ui::MainWindow window;
    std::map<std::string, json> disk;
    int writes = 0;
    Fixture() : window(services()) {
        window.setTextureInfo([](const std::string& p) { return fakeInfo(p); });
        window.setTexturePropsStore([this](const std::string& t) { return disk.count(t) ? disk[t] : json::object(); },
                                    [this](const std::string& t, const json& v) {
                                        disk[t] = v;
                                        ++writes;
                                        return std::string();
                                    });
    }
    ui::ReliefEditor* open(const std::string& texture = "dev/grey") {
        ui::ReliefEditor* e = window.openReliefEditor(texture, false);
        REQUIRE(e);
        e->view()->resize(720, 560);
        e->setView(0, 0);
        return e;
    }
};

QPointF px(ui::ReliefEditor* e, double x, double y, double z = 0) {
    const auto s = e->project(core::Vec3(x, y, z));
    REQUIRE(s);
    return QPointF(s->x, s->y);
}

void press(ui::ReliefEditor* e, int key, Qt::KeyboardModifiers m = Qt::NoModifier) { CHECK(e->key(key, m, QString())); }

double maxZ(const core::Brush& b) {
    double z = -1e9;
    for (const core::Vec3& v : b.vertices) z = std::max(z, v.z);
    return z;
}

} // namespace

TEST_CASE("relief editor: the tile has the texture's size in metres and starts flat; the projection that draws is the one that picks") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    CHECK(e->session().width() == doctest::Approx(2));
    CHECK(e->session().height() == doctest::Approx(1));
    CHECK(core::texplan::isFlatRelief(e->session().toRelief()));
    CHECK_FALSE(e->dirty());
    for (int view = 0; view < 2; ++view) {
        if (view == 1) e->setView(0.55, 0.3);
        const core::Vec3 p(1.3, 0.4, 0.2);
        const auto s = e->project(p);
        REQUIRE(s);
        const render::Ray ray = e->rayAt(s->x, s->y);
        const render::Vec3 d = render::Vec3{p.x, p.y, p.z} - ray.origin;
        const render::Vec3 off = d - ray.dir * render::dot(d, ray.dir);
        CHECK(render::length(off) < 1e-6);
    }
    // the whole tile is on screen after framing
    e->setView(0, 0);
    for (const core::Vec3& c : {core::Vec3(0, 0, 0), core::Vec3(2, 0, 0), core::Vec3(2, 1, 0), core::Vec3(0, 1, 0)}) {
        const QPointF q = px(e, c.x, c.y, c.z);
        CHECK(q.x() > 0);
        CHECK(q.x() < 720);
        CHECK(q.y() > 0);
        CHECK(q.y() < 560);
    }
    // front view: +x goes right and +y goes up the screen
    CHECK(px(e, 2, 0).x() > px(e, 0, 0).x());
    CHECK(px(e, 0, 1).y() < px(e, 0, 0).y());
}

TEST_CASE("relief editor: clicking picks the element under the pointer by the mode; shift adds, an empty spot clears; a box takes what is inside") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    e->setMode(mesh::SelMode::Vertex);
    const QPointF c1 = px(e, 2, 0);
    e->click(c1.x() + 2, c1.y() + 1, false);
    REQUIRE(e->session().sel().verts.size() == 1);
    CHECK(e->session().sel().verts[0] == 1);
    const QPointF c2 = px(e, 2, 1);
    e->click(c2.x(), c2.y(), true);
    CHECK(e->session().sel().verts.size() == 2);
    e->click(c2.x(), c2.y(), true);   // shift again toggles it off
    CHECK(e->session().sel().verts.size() == 1);
    e->click(3, 3, false);
    CHECK_FALSE(e->session().hasSelection());
    // edges
    e->setMode(mesh::SelMode::Edge);
    const QPointF a = px(e, 0, 0), b = px(e, 2, 0);
    e->click((a.x() + b.x()) / 2, a.y() + 2, false);
    CHECK(e->session().sel().edges.size() == 1);
    // faces
    e->setMode(mesh::SelMode::Face);
    const QPointF mid = px(e, 1, 0.5);
    e->click(mid.x(), mid.y(), false);
    CHECK(e->session().sel().faces.size() == 1);
    e->click(3, 3, false);
    CHECK(e->session().sel().faces.empty());
    // box
    e->setMode(mesh::SelMode::Vertex);
    e->boxSelect(a.x() - 10, a.y() + 10, a.x() + 10, a.y() - 10, false);
    CHECK(e->session().sel().verts.size() == 1);
    e->boxSelect(c2.x() - 10, c2.y() + 10, c2.x() + 10, c2.y() - 10, true);
    CHECK(e->session().sel().verts.size() == 2);
    e->boxSelect(0, 0, 720, 560, false);
    CHECK(e->session().sel().verts.size() == 4);
}

TEST_CASE("relief editor: Move with a typed number on an axis is one undo step; Esc puts everything back") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    e->setMouse(QPointF(360, 280));
    press(e, Qt::Key_1);
    press(e, Qt::Key_A);
    CHECK(e->session().sel().verts.size() == 4);
    press(e, Qt::Key_G);
    REQUIRE(e->modalActive());
    press(e, Qt::Key_Z);
    press(e, Qt::Key_0);
    press(e, Qt::Key_Period);
    press(e, Qt::Key_2);
    press(e, Qt::Key_5);
    CHECK(e->modalValue() == doctest::Approx(0.25));
    CHECK(e->status().contains(QStringLiteral("0.25")));
    press(e, Qt::Key_Return);
    CHECK_FALSE(e->modalActive());
    CHECK(e->session().brush().vertices[0].z == doctest::Approx(0.25));
    CHECK(e->dirty());
    e->undo();   // the whole move
    CHECK(e->session().brush().vertices[0].z == doctest::Approx(0));
    CHECK_FALSE(e->session().canUndo());
    // cancel
    press(e, Qt::Key_G);
    press(e, Qt::Key_Z);
    press(e, Qt::Key_1);
    press(e, Qt::Key_Escape);
    CHECK_FALSE(e->modalActive());
    CHECK(e->session().brush().vertices[0].z == doctest::Approx(0));
    CHECK_FALSE(e->dirty());
    // the keys of another mode do not run during a tool; a tool needs a selection
    e->session().selectNone();
    CHECK(e->key(Qt::Key_G, Qt::NoModifier, QString()));
    CHECK_FALSE(e->modalActive());
    CHECK(e->status().contains(QStringLiteral("Select something")));
}

TEST_CASE("relief editor: Extrude, Inset and Rotate / Scale run as modals from the base each time") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    e->setMouse(QPointF(360, 280));
    press(e, Qt::Key_3);
    press(e, Qt::Key_A);
    press(e, Qt::Key_E);
    REQUIRE(e->modalActive());
    for (int k : {Qt::Key_0, Qt::Key_Period, Qt::Key_3}) press(e, k);
    CHECK(e->modalValue() == doctest::Approx(0.3));
    press(e, Qt::Key_Backspace);
    press(e, Qt::Key_5);
    CHECK(e->modalValue() == doctest::Approx(0.5));   // the buffer was edited, the extrusion is not stacked
    press(e, Qt::Key_Return);
    CHECK(maxZ(e->session().brush()) == doctest::Approx(0.5));
    CHECK(e->session().brush().faces.size() > 1);
    e->undo();
    CHECK(e->session().brush().faces.size() == 1);
    // inset
    press(e, Qt::Key_3);
    press(e, Qt::Key_A);
    press(e, Qt::Key_I);
    REQUIRE(e->modalActive());
    for (int k : {Qt::Key_0, Qt::Key_Period, Qt::Key_2}) press(e, k);
    press(e, Qt::Key_Return);
    CHECK(e->session().brush().faces.size() == 5);
    e->undo();
    // rotate by 90 degrees about the surface normal: the tile corner stays inside the tile (clamped)
    press(e, Qt::Key_1);
    press(e, Qt::Key_A);
    press(e, Qt::Key_R);
    REQUIRE(e->modalActive());
    press(e, Qt::Key_9);
    press(e, Qt::Key_0);
    press(e, Qt::Key_Return);
    for (const core::Vec3& v : e->session().brush().vertices) {
        CHECK(v.x >= -1e-9);
        CHECK(v.x <= 2 + 1e-9);
        CHECK(v.y >= -1e-9);
        CHECK(v.y <= 1 + 1e-9);
    }
    e->undo();
    // scale
    press(e, Qt::Key_A);
    press(e, Qt::Key_S);
    REQUIRE(e->modalActive());
    press(e, Qt::Key_X);   // only along x
    press(e, Qt::Key_0);
    press(e, Qt::Key_Period);
    press(e, Qt::Key_5);
    press(e, Qt::Key_Return);
    CHECK(e->session().brush().vertices[1].x == doctest::Approx(1.5));   // 1 + (2 - 1) * 0.5
    CHECK(e->session().brush().vertices[1].y == doctest::Approx(0));
}

TEST_CASE("relief editor: bevel needs edges, the wheel adds segments; a loop cut follows the hovered edge and the typed fraction") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    e->setMouse(QPointF(360, 280));
    // a bevel with nothing selected
    CHECK_FALSE(e->startModal(native::tools::ModalKind::Bevel));
    // an inset gives edges to bevel: the corner edges between the border faces
    press(e, Qt::Key_3);
    press(e, Qt::Key_A);
    press(e, Qt::Key_I);
    for (int k : {Qt::Key_0, Qt::Key_Period, Qt::Key_2}) press(e, k);
    press(e, Qt::Key_Return);
    REQUIRE(e->session().brush().faces.size() == 5);
    press(e, Qt::Key_2);
    e->session().selectNone();
    const QPointF a = px(e, 0.2, 0.2), b = px(e, 0.2, 0.8);   // the inner left edge
    e->click(a.x(), (a.y() + b.y()) / 2, false);
    REQUIRE(e->session().sel().edges.size() == 1);
    press(e, Qt::Key_B, Qt::ControlModifier);
    if (e->modalActive()) {
        CHECK(e->inModal(native::tools::ModalKind::Bevel));
        CHECK(e->modalSegments() == 1);
        e->modalWheel(-120);
        CHECK(e->modalSegments() == 2);
        e->modalWheel(120);
        CHECK(e->modalSegments() == 1);
        press(e, Qt::Key_Escape);
    }
    CHECK_FALSE(e->modalActive());
    // loop cut on the flat quad: hover the bottom edge
    e->undo();
    REQUIRE(e->session().brush().faces.size() == 1);
    const QPointF lo0 = px(e, 0, 0), lo1 = px(e, 2, 0);
    const QPointF onEdge((lo0.x() + lo1.x()) / 2, lo0.y() + 1);
    e->armLoopCut(true);
    CHECK(e->loopCutArmed());
    e->hover(onEdge.x(), onEdge.y());
    CHECK_FALSE(e->loopOverlay().empty());
    REQUIRE(e->startModal(native::tools::ModalKind::LoopCut));
    CHECK_FALSE(e->loopCutArmed());
    for (int k : {Qt::Key_0, Qt::Key_Period, Qt::Key_5}) press(e, k);
    press(e, Qt::Key_Return);
    CHECK(e->session().brush().faces.size() == 2);
    e->undo();
    CHECK(e->session().brush().faces.size() == 1);
}

TEST_CASE("relief editor: presets, depth of the selection and the report") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    e->loadPreset(rl::Preset::Recess);
    CHECK(e->session().brush().faces.size() > 1);
    CHECK(e->dirty());
    CHECK(e->report().warnings.empty());
    CHECK(e->report().minZ < 0);
    e->undo();
    CHECK(core::texplan::isFlatRelief(e->session().toRelief()));
    CHECK_FALSE(e->dirty());
    // the depth of the picked vertices; a raised border is reported
    press(e, Qt::Key_1);
    press(e, Qt::Key_A);
    e->setDepth(0.4);
    CHECK(e->session().brush().vertices[2].z == doctest::Approx(0.4));
    CHECK_FALSE(e->report().warnings.empty());
    CHECK(static_cast<QDoubleSpinBox*>(e->part(QStringLiteral("spin:depth")))->value() == doctest::Approx(0.4));
    e->setDepth(0);
    CHECK(e->report().warnings.empty());
    // the buttons do the same
    static_cast<QPushButton*>(e->part(QStringLiteral("btn:preset:raise")))->click();
    CHECK(e->report().maxZ > 0);
    static_cast<QPushButton*>(e->part(QStringLiteral("btn:undo")))->click();
    CHECK(e->report().maxZ == doctest::Approx(0));   // the click on Undo took the preset back
}

TEST_CASE("relief editor: the faces show the texture, the neighbours are dimmed, and the picture can be switched off") {
    Fixture f;
    ui::ReliefEditor* e = f.open();
    QImage red(8, 8, QImage::Format_RGBA8888);
    red.fill(QColor(220, 30, 30));
    e->setImage(red);
    const QPointF mid = px(e, 1, 0.5);
    QImage shot = e->view()->grab().toImage();
    const auto at = [&](const QImage& img, const QPointF& p) {   // the picture may be scaled by the screen's pixel ratio
        const double k = static_cast<double>(img.width()) / e->view()->width();
        return img.pixelColor(static_cast<int>(p.x() * k), static_cast<int>(p.y() * k));
    };
    REQUIRE_FALSE(shot.isNull());
    QColor c = at(shot, mid);
    CHECK(c.red() > 150);
    CHECK(c.green() < 90);
    // a neighbouring repeat to the right of the tile is drawn, but dimmer
    const QPointF ghost = px(e, 3, 0.5);
    if (ghost.x() < 720) {
        const QColor g = at(shot, ghost);
        CHECK(g.red() > g.green());
        CHECK(g.red() < c.red());
    }
    // texture off: the faces are plain grey
    static_cast<QCheckBox*>(e->part(QStringLiteral("chk:texture")))->setChecked(false);
    CHECK_FALSE(e->showTexture());
    shot = e->view()->grab().toImage();
    c = at(shot, mid);
    CHECK(std::abs(c.red() - c.green()) < 30);
    // a preset with raised faces draws without trouble from the side and the top as well
    e->loadPreset(rl::Preset::Window);
    for (const auto& v : {std::pair<double, double>{0, 0}, {1.57, 0}, {0, 1.5}, {0.55, 0.3}}) {
        e->setView(v.first, v.second);
        CHECK_FALSE(e->view()->grab().toImage().isNull());
    }
}

TEST_CASE("relief editor: saving writes the relief into the sidecar, a flat mesh removes it, closing without saving writes nothing") {
    Fixture f;
    f.disk["dev/grey"] = json{{"pixelsPerMeter", 64}};
    ui::ReliefEditor* e = f.open();
    e->loadPreset(rl::Preset::Raise);
    CHECK(e->dirty());
    CHECK(e->save().empty());
    CHECK(f.writes == 1);
    REQUIRE(f.disk["dev/grey"].contains("relief"));
    CHECK(f.disk["dev/grey"]["relief"]["faces"].size() > 1);
    CHECK(f.disk["dev/grey"]["pixelsPerMeter"] == 64);   // the other keys stay
    CHECK_FALSE(e->dirty());
    // the file reads back to the same mesh in a new editor
    ui::ReliefEditor* again = f.open();
    CHECK(again->session().brush().faces.size() == e->session().brush().faces.size());
    CHECK_FALSE(again->dirty());
    // flat: the relief goes away
    again->session().reset();
    CHECK(again->dirty());
    CHECK(again->save().empty());
    CHECK((!f.disk["dev/grey"].contains("relief") || f.disk["dev/grey"]["relief"].is_null()));
    // discard writes nothing
    const int before = f.writes;
    ui::ReliefEditor* third = f.open();
    third->loadPreset(rl::Preset::Recess);
    third->discard();
    CHECK(f.writes == before);
    // apply saves and closes
    ui::ReliefEditor* fourth = f.open();
    fourth->loadPreset(rl::Preset::Recess);
    CHECK(fourth->applyAndClose());
    CHECK(f.writes == before + 1);
    CHECK(f.disk["dev/grey"].contains("relief"));
    // remove relief
    ui::ReliefEditor* fifth = f.open();
    CHECK_FALSE(fifth->session().brush().faces.size() == 1);
    fifth->removeRelief();
    CHECK((!f.disk["dev/grey"].contains("relief") || f.disk["dev/grey"]["relief"].is_null()));
}

TEST_CASE("relief editor: the Texture Properties window opens it, and takes the saved relief over so it cannot write the old one back") {
    Fixture f;
    f.disk["dev/grey"] = json{{"pixelsPerMeter", 64}};
    ui::TexturePropsDialog* d = f.window.openTextureProperties("dev/grey", false);
    REQUIRE(d);
    auto* btn = static_cast<QPushButton*>(d->part(QStringLiteral("btn:relief")));
    REQUIRE(btn);
    btn->click();
    ui::ReliefEditor* e = f.window.reliefEditor();
    REQUIRE(e);
    CHECK(e->texture() == "dev/grey");
    e->loadPreset(rl::Preset::Recess);
    CHECK(e->save().empty());
    CHECK(d->props().contains("relief"));
    CHECK_FALSE(d->props()["relief"].is_null());
    CHECK(d->flush().empty());
    CHECK(f.disk["dev/grey"].contains("relief"));   // still there after the dialog wrote its own copy
    CHECK(f.disk["dev/grey"]["relief"]["faces"].size() > 1);
}
