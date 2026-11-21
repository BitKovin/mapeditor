// T11.4b: the texture group editor window (offscreen, never shown): text <-> parameters, slots, problems and preview; Save and Apply through a fake group store (no assets folder).
#include <doctest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>

#include <map>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"
#include "tools/texturing/group_tools.h"
#include "ui/app_actions.h"
#include "ui/groups_panel.h"
#include "ui/main_window.h"
#include "ui/texgroup_editor.h"

using namespace native;
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
        i.height = 64;
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
    int writes = 0;
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
                                     ++writes;
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
T* part(ui::TexGroupEditor* d, const char* name) {
    return static_cast<T*>(d->part(QString::fromLatin1(name)));
}

template <class T>
T* part2(ui::GroupsPanel* d, const char* name) {
    return static_cast<T*>(d->part(QString::fromLatin1(name)));
}

} // namespace

TEST_CASE("group editor: a new group starts from the first template, which parses, lays out and shows its parameters and slots") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    REQUIRE(d);
    CHECK(d->valid());
    CHECK(d->errors().empty());
    CHECK_FALSE(d->preview()->cells().empty());
    CHECK(part<QListWidget>(d, "slots")->count() == 6);
    REQUIRE(part<QDoubleSpinBox>(d, "param:floorHeight"));
    CHECK(part<QDoubleSpinBox>(d, "param:floorHeight")->value() == doctest::Approx(3));
    CHECK(part<QDoubleSpinBox>(d, "param:bayWidth"));
    CHECK(part<QLineEdit>(d, "name")->text().isEmpty());
    CHECK_FALSE(part<QLineEdit>(d, "name")->isReadOnly());
    // the templates replace the text
    auto* tpl = part<QComboBox>(d, "template");
    d->setText(QStringLiteral("{"));
    CHECK_FALSE(d->valid());
    CHECK(tpl->count() == 4);
}

TEST_CASE("group editor: a parameter changes the layout; a broken text keeps the controls and shows the error; fixing it brings the preview back") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    const size_t before = d->preview()->cells().size();
    part<QDoubleSpinBox>(d, "param:floorHeight")->setValue(6);
    const size_t after = d->preview()->cells().size();
    CHECK(after != before);
    const QString good = d->text();
    d->setText(QStringLiteral("{ \"root\": "));
    CHECK_FALSE(d->valid());
    REQUIRE_FALSE(d->errors().empty());
    CHECK(d->preview()->cells().empty());
    CHECK(part<QDoubleSpinBox>(d, "param:floorHeight"));   // the controls stay
    CHECK(part<QListWidget>(d, "problems")->item(0)->text().contains(QStringLiteral("JSON")));
    d->setText(good);
    CHECK(d->valid());
    CHECK(d->preview()->cells().size() == after);   // the parameter value was kept
    CHECK(part<QDoubleSpinBox>(d, "param:floorHeight")->value() == doctest::Approx(6));
    // a group the reader refuses
    d->setText(QStringLiteral(R"({"format": 1, "slots": {"a": "dev/grey"}, "root": {"split": "q", "parts": []}})"));
    CHECK_FALSE(d->valid());
    CHECK_FALSE(d->errors().empty());
}

TEST_CASE("group editor: the facing and the size change the layout; the seed changes random groups") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    d->setText(QString::fromStdString(native::tools::tex::groupTemplates()[1].text));   // panels: random panels
    REQUIRE(d->valid());
    std::vector<std::string> a, b;
    for (const auto& c : d->preview()->cells()) a.push_back(c.texture);
    part<QSpinBox>(d, "seed")->setValue(12345);
    for (const auto& c : d->preview()->cells()) b.push_back(c.texture);
    CHECK(a.size() == b.size());
    CHECK(a != b);
    part<QDoubleSpinBox>(d, "width")->setValue(3);
    CHECK(d->preview()->cells().size() < a.size());
    part<QComboBox>(d, "facing")->setCurrentIndex(1);
    CHECK(d->valid());
    CHECK_FALSE(d->preview()->cells().empty());
}

TEST_CASE("group editor: the preview finds the cell under a point; hover shows the rule, a click marks and a second click unmarks") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    ui::GroupPreviewCanvas* cv = d->preview();
    cv->resize(520, 440);
    REQUIRE_FALSE(cv->cells().empty());
    const auto& c0 = cv->cells().front();
    const QPointF p = cv->pixelOf((c0.x0 + c0.x1) / 2, (c0.y0 + c0.y1) / 2);
    const int i = cv->cellAtPixel(p);
    REQUIRE(i >= 0);
    CHECK(cv->cells()[static_cast<size_t>(i)].path == cv->cells()[static_cast<size_t>(cv->cellAtPixel(p))].path);
    cv->hover(p);
    CHECK(cv->hovered() == i);
    CHECK(part<QLabel>(d, "info")->text().contains(QStringLiteral("rule path")));
    cv->click(p);
    CHECK(cv->highlight() == i);
    cv->click(p);
    CHECK(cv->highlight() == -1);
    cv->hover(QPointF(-5, -5));
    CHECK(cv->hovered() == -1);
}

TEST_CASE("group editor: a slot gets another texture, the text is reformatted and the slot list follows") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    auto* slotList = part<QListWidget>(d, "slots");
    slotList->setCurrentRow(1);   // window
    part<QLineEdit>(d, "slotTexture")->setText(QStringLiteral("wall/brick"));
    static_cast<QPushButton*>(d->part("btn:slotSet"))->click();
    CHECK(d->text().contains(QStringLiteral("wall/brick")));
    CHECK(slotList->item(1)->text().contains(QStringLiteral("wall/brick")));
    CHECK(d->valid());
    // the active texture
    slotList->setCurrentRow(0);
    f.window.setActiveTexture("dev/metal");
    static_cast<QPushButton*>(d->part("btn:slotActive"))->click();
    CHECK(slotList->item(0)->text().contains(QStringLiteral("dev/metal")));
    // a broken text cannot change a slot
    d->setText(QStringLiteral("{"));
    d->setSlot("wall", "x/y");
    CHECK(d->status().contains(QStringLiteral("valid JSON")));
}

TEST_CASE("group editor: save checks the name and the text, writes through the store and then the name is fixed") {
    Fixture f;
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    CHECK_FALSE(d->save().empty());   // no name
    CHECK(f.writes == 0);
    d->setName(QStringLiteral("bad name"));
    CHECK_FALSE(d->save().empty());
    d->setName(QStringLiteral("bld/test"));
    d->setText(QStringLiteral("{ \"root\": "));
    CHECK_FALSE(d->save().empty());
    CHECK(f.writes == 0);
    d->setText(QString::fromStdString(native::tools::tex::groupTemplates()[2].text));
    CHECK(d->save().empty());
    CHECK(f.writes == 1);
    REQUIRE(f.disk.count("bld/test"));
    CHECK(f.disk["bld/test"]["name"] == "Bordered floor");
    CHECK(d->status().contains(QStringLiteral("Saved")));
    CHECK(part<QLineEdit>(d, "name")->isReadOnly());
    CHECK(d->groupName() == "bld/test");
    // the group can be opened again from the store
    ui::TexGroupEditor* e = f.window.openTexGroupEditor("bld/test", false);
    CHECK(e->valid());
    CHECK(part<QLineEdit>(e, "name")->text() == "bld/test");
    CHECK(part<QLineEdit>(e, "name")->isReadOnly());
    CHECK(e->text().contains(QStringLiteral("Bordered floor")));
    // a group that cannot be read opens as a template with a message
    ui::TexGroupEditor* m = f.window.openTexGroupEditor("gone/missing", false);
    CHECK(m->status().contains(QStringLiteral("Could not read")));
}

TEST_CASE("group editor: a store error is reported and nothing is claimed saved") {
    Fixture f;
    f.window.setTexGroupStore({nullptr, nullptr, [](const std::string&, const json&) { return std::string("disk full"); }, nullptr});
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    d->setName(QStringLiteral("a"));
    CHECK(d->save() == "disk full");
    CHECK(d->status().contains(QStringLiteral("disk full")));
    CHECK_FALSE(part<QLineEdit>(d, "name")->isReadOnly());
}

TEST_CASE("apply group: the selected faces get the layout in one undo step; the editor's Apply saves first") {
    Fixture f;
    f.disk["bld/set"] = json::parse(native::tools::tex::groupTemplates()[0].text);
    CHECK_FALSE(f.window.applyTexGroup("bld/set"));   // nothing selected
    f.window.selection().select(doc::SelRef::brush("w", "a"));
    CHECK(f.window.applyTexGroup("bld/set"));
    CHECK(f.layouts() == 6);
    // the editor
    f.window.selection().select(doc::SelRef::face("w", "a", f.window.document()->entities[0]->brushes[0]->brush.faces[0].id));
    ui::TexGroupEditor* d = f.window.openTexGroupEditor({}, false);
    d->setName(QStringLiteral("new/one"));
    CHECK(d->saveAndApply().empty());
    CHECK(f.disk.count("new/one"));
    CHECK(d->status().contains(QStringLiteral("applied")));
    bool found = false;
    for (const core::BrushFace& face : f.window.document()->entities[0]->brushes[0]->brush.faces)
        if (native::tools::tex::layoutGroupOf(face) == "new/one") found = true;
    CHECK(found);
}

TEST_CASE("group store: writing and removing rebuild the scene; the list and read go through the store") {
    Fixture f;
    CHECK(f.window.texGroupList().empty());
    const json g = json::parse(native::tools::tex::groupTemplates()[1].text);
    CHECK(f.window.texGroupWrite("a/b", g).empty());
    CHECK(f.window.texGroupList() == std::vector<std::string>{"a/b"});
    REQUIRE(f.window.texGroupRead("a/b"));
    CHECK((*f.window.texGroupRead("a/b"))["name"] == "Panel wall");
    CHECK_FALSE(f.window.texGroupRead("x"));
    CHECK(f.window.texGroupRemove("a/b").empty());
    CHECK(f.window.texGroupList().empty());
}

TEST_CASE("groups panel: two tabs by the kind of file, a search, thumbnails, apply / duplicate / remove through the store") {
    Fixture f;
    f.disk["bld/facade"] = json::parse(native::tools::tex::groupTemplates()[0].text);
    f.disk["bld/panels"] = json::parse(native::tools::tex::groupTemplates()[1].text);
    f.disk["sets/wall"] = json{{"format", 1}, {"name", "Wall set"}, {"kit", {{"kind", "wall"}}}, {"root", {{"tex", "a"}}}};
    ui::GroupsPanel* p = f.window.groupsPanel();
    REQUIRE(p);
    p->reload();
    CHECK(p->entries().size() == 3);
    CHECK(p->tab() == native::tools::tex::GroupTab::Advanced);
    CHECK(p->shown() == std::vector<std::string>{"bld/facade", "bld/panels"});
    p->setTab(native::tools::tex::GroupTab::Simple);
    CHECK(p->shown() == std::vector<std::string>{"sets/wall"});
    p->setTab(native::tools::tex::GroupTab::Advanced);
    p->setQuery(QStringLiteral("PAN"));
    CHECK(p->shown() == std::vector<std::string>{"bld/panels"});
    CHECK(part2<QListWidget>(p, "list")->count() == 1);
    CHECK(part2<QLabel>(p, "count")->text() == "1 of 2");
    p->setQuery({});
    // the thumbnail is the group laid out (not the empty grey)
    const QImage thumb = p->thumbnailOf("bld/facade");
    CHECK(thumb.size() == QSize(96, 72));
    bool varied = false;
    for (int x = 0; x < thumb.width() && !varied; ++x)
        if (thumb.pixel(x, 30) != thumb.pixel(0, 0)) varied = true;
    CHECK(varied);
    // apply
    f.window.selection().select(doc::SelRef::brush("w", "a"));
    CHECK(p->apply("bld/facade"));
    CHECK(f.layouts() == 6);
    // duplicate: a new file with a new name, the list follows (the write signals the panel)
    CHECK(p->duplicate("bld/facade") == "bld/facade_copy");
    CHECK(f.disk.count("bld/facade_copy"));
    CHECK(f.disk["bld/facade_copy"]["name"] == "Building facade (copy)");
    CHECK(p->duplicate("bld/facade") == "bld/facade_copy2");
    CHECK(p->shown().size() == 4);
    CHECK(p->remove("bld/facade_copy").empty());
    CHECK_FALSE(f.disk.count("bld/facade_copy"));
    CHECK(p->shown().size() == 3);
    // edit opens the editor on the group; saving it refreshes the panel
    p->edit("bld/panels");
    REQUIRE(f.window.groupEditor());
    CHECK(f.window.groupEditor()->groupName() == "bld/panels");
}

TEST_CASE("groups panel: the New advanced group button opens the editor on a template; the dock exists and has an action") {
    Fixture f;
    ui::GroupsPanel* p = f.window.groupsPanel();
    REQUIRE(p);
    CHECK(f.window.groupsDock());
    static_cast<QPushButton*>(p->part("btn:newAdvanced"))->click();
    REQUIRE(f.window.groupEditor());
    CHECK(f.window.groupEditor()->groupName().empty());
    CHECK(f.window.groupEditor()->valid());
    CHECK(f.window.registry().find("view.groups.toggle"));
}
