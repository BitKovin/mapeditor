// T11.4d: the tile set editor window (offscreen, never shown): wall lanes and pieces, the inspector, frame slots, options, the preview and Save / Apply through a fake group store.
#include <doctest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>

#include <map>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"
#include "ui/app_actions.h"
#include "ui/groups_panel.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"
#include "ui/tileset_editor.h"

using namespace native;
using namespace native::tools::tex;
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
        i.width = 128;
        i.height = 128;
        i.ppm = 64;
        i.props = tp::defaultProps();
        it = infos.emplace(path, i).first;
    }
    return &it->second;
}

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {4, 2, 3}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

struct Fixture {
    ui::MainWindow window;
    std::map<std::string, json> disk;
    Fixture() : window(services()) {
        window.setTextureInfo([](const std::string& p) { return fakeInfo(p); });
        window.setTexGroupStore({[this] {
                                     std::vector<std::string> n;
                                     for (const auto& [k, v] : disk) n.push_back(k);
                                     return n;
                                 },
                                 [this](const std::string& n) -> std::optional<json> {
                                     const auto it = disk.find(n);
                                     if (it == disk.end()) return std::nullopt;
                                     return it->second;
                                 },
                                 [this](const std::string& n, const json& g) {
                                     disk[n] = g;
                                     return std::string();
                                 },
                                 [this](const std::string& n) {
                                     disk.erase(n);
                                     return std::string();
                                 }});
        window.setDocument(oneBox(), true);
    }
    int layouts() const {
        int n = 0;
        for (const auto& e : window.document()->entities)
            for (const auto& b : e->brushes)
                for (const core::BrushFace& f : b->brush.faces)
                    if (f.look && f.look->contains("layout")) ++n;
        return n;
    }
};

template <class T>
T* part(ui::TileSetEditor* d, const char* name) {
    return static_cast<T*>(d->part(QString::fromLatin1(name)));
}

} // namespace

TEST_CASE("tile set editor: a new wall set is empty and not usable; adding textures sorts them into lanes and the preview lays them out") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    REQUIRE(d);
    CHECK_FALSE(d->valid());
    CHECK(d->preview()->cells().empty());
    CHECK(part<QListWidget>(d, "lane:middle")->count() == 0);
    CHECK(d->addTextures({"dev/roof", "dev/wall", "dev/metal", "dev/door_1", "dev/window_1"}) == 7);   // the plain pieces are also copied to the bottom lane that has only a door
    CHECK(d->valid());
    CHECK(part<QListWidget>(d, "lane:top")->count() == 1);
    CHECK(part<QListWidget>(d, "lane:middle")->count() == 3);
    CHECK(part<QListWidget>(d, "lane:bottom")->count() == 3);   // the door and the two plain pieces
    CHECK_FALSE(d->preview()->cells().empty());
    CHECK(d->status().contains(QStringLiteral("7 pieces added")));
}

TEST_CASE("tile set editor: the inspector edits the selected piece: tiles, size, how often, where, match, lane, duplicate, delete") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    f.window.setActiveTexture("dev/metal");
    CHECK(d->addPiece(Lane::Middle, "dev/wall") == 0);
    CHECK(part<QWidget>(d, "piece")->isVisibleTo(d));
    part<QSpinBox>(d, "wide")->setValue(2);
    REQUIRE(d->kit().lane(Lane::Middle).pieces[0].cols() == 2);
    d->selectCell(0, 1);
    static_cast<QPushButton*>(d->part("btn:useActive"))->click();
    CHECK(d->kit().lane(Lane::Middle).pieces[0].cells[0][1] == "dev/metal");
    CHECK(part<QLineEdit>(d, "texture")->text() == "dev/metal");
    part<QLineEdit>(d, "texture")->setText(QStringLiteral("dev/grey"));
    static_cast<QPushButton*>(d->part("btn:set"))->click();
    CHECK(d->kit().lane(Lane::Middle).pieces[0].cells[0][1] == "dev/grey");
    static_cast<QPushButton*>(d->part("btn:clear"))->click();
    CHECK_FALSE(d->kit().lane(Lane::Middle).pieces[0].cells[0][1]);
    part<QLineEdit>(d, "texture")->setText(QStringLiteral("../x"));
    static_cast<QPushButton*>(d->part("btn:set"))->click();
    CHECK(d->status().contains(QStringLiteral("valid")));
    d->setTargetTexture("dev/grey");
    CHECK(d->valid());
    // tall changes the whole lane
    part<QSpinBox>(d, "tall")->setValue(2);
    CHECK(d->kit().lane(Lane::Middle).rows == 2);
    CHECK(d->kit().lane(Lane::Middle).pieces[0].rows() == 2);
    // how often, where, match, lane
    part<QComboBox>(d, "weight")->setCurrentIndex(0);
    emit part<QComboBox>(d, "weight")->activated(0);
    CHECK(d->kit().lane(Lane::Middle).pieces[0].weight == doctest::Approx(kWeightRare));
    part<QComboBox>(d, "place")->setCurrentIndex(4);
    emit part<QComboBox>(d, "place")->activated(4);
    CHECK(d->kit().lane(Lane::Middle).pieces[0].place == Place::Once);
    part<QLineEdit>(d, "match")->setText(QStringLiteral("win 1"));
    emit part<QLineEdit>(d, "match")->editingFinished();
    CHECK(d->kit().lane(Lane::Middle).pieces[0].match == "win 1");
    part<QLineEdit>(d, "match")->setText(QStringLiteral("bad/name"));
    emit part<QLineEdit>(d, "match")->editingFinished();
    CHECK_FALSE(d->kit().lane(Lane::Middle).pieces[0].match);
    part<QComboBox>(d, "lane")->setCurrentIndex(2);
    emit part<QComboBox>(d, "lane")->activated(2);
    CHECK(d->kit().lane(Lane::Middle).pieces.empty());
    REQUIRE(d->kit().lane(Lane::Bottom).pieces.size() == 1);
    // duplicate / delete
    static_cast<QPushButton*>(d->part("btn:duplicate"))->click();
    CHECK(d->kit().lane(Lane::Bottom).pieces.size() == 2);
    static_cast<QPushButton*>(d->part("btn:delete"))->click();
    CHECK(d->kit().lane(Lane::Bottom).pieces.size() == 1);
    CHECK_FALSE(part<QWidget>(d, "piece")->isVisibleTo(d));   // nothing selected
}

TEST_CASE("tile set editor: lane buttons add the active texture and change the tile rows; the lists follow the selection") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    f.window.setActiveTexture("dev/floor");
    static_cast<QPushButton*>(d->part("btn:add:top"))->click();
    CHECK(d->kit().lane(Lane::Top).pieces.size() == 1);
    static_cast<QPushButton*>(d->part("btn:rows+:top"))->click();
    CHECK(d->kit().lane(Lane::Top).rows == 2);
    CHECK(part<QLabel>(d, "rows:top")->text() == "2 tile rows");
    static_cast<QPushButton*>(d->part("btn:rows-:top"))->click();
    CHECK(d->kit().lane(Lane::Top).rows == 1);
    CHECK(part<QListWidget>(d, "lane:top")->currentRow() == 0);
    static_cast<QPushButton*>(d->part("btn:add:middle"))->click();
    static_cast<QPushButton*>(d->part("btn:add:middle"))->click();
    part<QListWidget>(d, "lane:middle")->setCurrentRow(0);
    CHECK(part<QComboBox>(d, "lane")->currentIndex() == 1);
}

TEST_CASE("tile set editor: the options change the kit and the preview") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    d->addTextures({"dev/wall"});
    part<QCheckBox>(d, "tileAuto")->setChecked(false);
    REQUIRE(d->kit().tile);
    part<QDoubleSpinBox>(d, "tileW")->setValue(2);
    part<QDoubleSpinBox>(d, "tileH")->setValue(3);
    CHECK((*d->kit().tile)[0] == doctest::Approx(2));
    CHECK((*d->kit().tile)[1] == doctest::Approx(3));
    part<QSpinBox>(d, "maxStretch")->setValue(0);
    CHECK(d->kit().maxStretch == doctest::Approx(0));
    part<QCheckBox>(d, "avoidRepeat")->setChecked(false);
    CHECK_FALSE(d->kit().avoidRepeat);
    part<QCheckBox>(d, "tileAuto")->setChecked(true);
    CHECK_FALSE(d->kit().tile);
    const size_t before = d->preview()->cells().size();
    part<QDoubleSpinBox>(d, "width")->setValue(3);
    CHECK(d->preview()->cells().size() != before);
}

TEST_CASE("tile set editor: a frame has nine slots; it is usable once any are chosen") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Frame, false);
    REQUIRE(d);
    CHECK(d->kit().kind == KitKind::Frame);
    CHECK_FALSE(d->valid());
    for (int i = 0; i < 9; ++i) {
        REQUIRE(d->part(QString::fromLatin1("slot:") + kFrameSlots[i]));
        d->selectSlot(i);
        d->setTargetTexture("dev/grey");
    }
    CHECK(d->valid());
    CHECK_FALSE(d->preview()->cells().empty());
    d->selectSlot(4);
    static_cast<QPushButton*>(d->part("btn:clear"))->click();
    CHECK_FALSE(d->kit().frame[4]);
    // piece commands on a frame do nothing
    CHECK(d->addPiece(Lane::Middle, "dev/x") == -1);
    CHECK(d->addTextures({"dev/x"}) == 0);
}

TEST_CASE("tile set editor: save checks the name and the set, writes the group file with its kit and then the name is fixed; the file opens again") {
    Fixture f;
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    CHECK_FALSE(d->save().empty());   // no name
    d->setName(QStringLiteral("sets/brick"));
    CHECK_FALSE(d->save().empty());   // nothing in the set
    CHECK(f.disk.empty());
    d->addTextures({"dev/wall", "dev/door_1", "dev/roof"});
    part<QLineEdit>(d, "title")->setText(QStringLiteral("Brick building"));
    CHECK(d->save().empty());
    REQUIRE(f.disk.count("sets/brick"));
    CHECK(f.disk["sets/brick"]["name"] == "Brick building");
    CHECK(f.disk["sets/brick"]["kit"]["kind"] == "wall");
    CHECK(f.disk["sets/brick"]["kit"]["top"]["pieces"].size() == 1);
    CHECK(part<QLineEdit>(d, "name")->isReadOnly());
    ui::TileSetEditor* e = f.window.openTileSetEditor("sets/brick", KitKind::Wall, false);
    CHECK(e->valid());
    CHECK(e->kit().lane(Lane::Top).pieces.size() == 1);
    CHECK(e->kit().lane(Lane::Bottom).pieces[0].place == Place::Once);
    CHECK(part<QLineEdit>(e, "title")->text() == "Brick building");
    // a rule group is not a set
    f.disk["rules/x"] = json{{"format", 1}, {"root", {{"tex", "a"}}}};
    ui::TileSetEditor* r = f.window.openTileSetEditor("rules/x", KitKind::Wall, false);
    CHECK(r->status().contains(QStringLiteral("not a simple set")));
}

TEST_CASE("tile set editor: save and apply lays the set over the selection; the Groups panel lists it under Simple and opens it in this editor") {
    Fixture f;
    f.window.selection().select(doc::SelRef::brush("w", "a"));
    ui::TileSetEditor* d = f.window.openTileSetEditor({}, KitKind::Wall, false);
    d->addTextures({"dev/wall", "dev/roof"});
    d->setName(QStringLiteral("sets/w"));
    CHECK(d->saveAndApply().empty());
    CHECK(d->status().contains(QStringLiteral("applied")));
    // a wall set leaves the roof and floor faces of a whole brush alone: four of six faces
    CHECK(f.layouts() == 4);
    ui::GroupsPanel* p = f.window.groupsPanel();
    CHECK(p->shown() == std::vector<std::string>{"sets/w"});   // the Simple tab is the first
    p->edit("sets/w");
    REQUIRE(f.window.tileSetEditor());
    CHECK(f.window.tileSetEditor()->groupName() == "sets/w");
    static_cast<QPushButton*>(p->part("btn:newFrame"))->click();
    REQUIRE(f.window.tileSetEditor());
    CHECK(f.window.tileSetEditor()->kit().kind == KitKind::Frame);
    static_cast<QPushButton*>(p->part("btn:newWall"))->click();
    CHECK(f.window.tileSetEditor()->kit().kind == KitKind::Wall);
}
