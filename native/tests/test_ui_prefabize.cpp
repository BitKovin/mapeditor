// T11.7a: Turn into prefab in the main window (offscreen, never shown): the file under the prefab folder, the swap for one instance, one undo step, the name prompt and the
// overwrite question.
#include <doctest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"
#include "ui/place_panel.h"
#include <QListWidget>

using namespace native;
namespace fs = std::filesystem;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> twoBoxes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {10, 0, 0}, {12, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {14, 0, 0}, {16, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

struct Fixture {
    QTemporaryDir dir;
    ui::MainWindow window;
    int asked = 0;
    int confirms = 0;
    bool yes = true;
    std::optional<std::string> answer = std::string("props/crate");
    Fixture() : window(services()) {
        REQUIRE(dir.isValid());
        window.setAssetsRoot(dir.path().toStdString());
        window.setDocument(twoBoxes(), true);
        window.setNameHandler([this](const QString&, const QString&, const QString&) {
            ++asked;
            return answer;
        });
        window.setConfirmHandler([this](const QString&, const QString&) {
            ++confirms;
            return yes;
        });
        window.selection().select(doc::SelRef::brush("w", "a"));
    }
    fs::path file(const std::string& rel) const { return fs::path(dir.path().toStdString()) / rel; }
};

} // namespace

TEST_CASE("turn into prefab: the file is written, the selection becomes one instance, one undo brings it back") {
    Fixture f;
    CHECK(f.window.runAction("edit.prefabize"));
    CHECK(f.asked == 1);
    const fs::path file = f.file("prefabs/props/crate.map.json");
    REQUIRE(fs::exists(file));
    const doc::LoadResult saved = doc::loadDocumentFile(file, "prefabs/props/crate.map.json");
    INFO(saved.error);
    REQUIRE(saved.version);
    CHECK(saved.version->findBrush("w", "a") >= 0);
    const auto doc = f.window.document();
    REQUIRE(doc->instances.size() == 1);
    CHECK(doc->instances[0]->typed.file == "prefabs/props/crate.map.json");
    CHECK(doc->findBrush("w", "a") < 0);
    CHECK(doc->findBrush("w", "b") >= 0);
    REQUIRE(f.window.selection().items().size() == 1);
    CHECK(f.window.selection().items()[0] == doc::SelRef::instance(doc->instances[0]->typed.id));
    f.window.undo();
    CHECK(f.window.document()->instances.empty());
    CHECK(f.window.document()->findBrush("w", "a") >= 0);
}

TEST_CASE("turn into prefab: a name given with the action needs no prompt; a cancelled prompt changes nothing") {
    Fixture f;
    f.answer.reset();
    const auto before = f.window.document();
    CHECK_FALSE(f.window.turnIntoPrefab());
    CHECK(f.asked == 1);
    CHECK(f.window.document() == before);
    CHECK(f.window.turnIntoPrefab("crate"));
    CHECK(f.asked == 1);
    CHECK(fs::exists(f.file("prefabs/crate.map.json")));
}

TEST_CASE("turn into prefab: an existing file is replaced only after a yes") {
    Fixture f;
    fs::create_directories(f.file("prefabs"));
    std::ofstream(f.file("prefabs/crate.map.json")) << "keep me";
    f.yes = false;
    const auto before = f.window.document();
    CHECK_FALSE(f.window.turnIntoPrefab("crate"));
    CHECK(f.confirms == 1);
    CHECK(f.window.document() == before);
    {
        std::ifstream in(f.file("prefabs/crate.map.json"));
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text == "keep me");
    }
    f.yes = true;
    CHECK(f.window.turnIntoPrefab("crate"));
    CHECK(f.confirms == 2);
    CHECK(doc::loadDocumentFile(f.file("prefabs/crate.map.json"), "prefabs/crate.map.json").version);
}

TEST_CASE("turn into prefab: a bad name, no game folder or nothing selected changes nothing") {
    Fixture f;
    CHECK_FALSE(f.window.turnIntoPrefab("../evil"));
    CHECK(f.window.document()->instances.empty());
    f.window.selection().clear();
    CHECK_FALSE(f.window.turnIntoPrefab("crate"));
    CHECK_FALSE(f.window.runAction("edit.prefabize"));   // disabled without a selection
    CHECK(f.asked == 0);
}

TEST_CASE("place panel: prefabs get a picture and a size, one prefab that cannot be read says why") {
    Fixture f;
    REQUIRE(f.window.turnIntoPrefab("crate"));   // the file appears: the Place panel scans again
    fs::create_directories(f.file("prefabs"));
    std::ofstream(f.file("prefabs/broken.map.json")) << "{ not json";
    ui::PlacePanel* place = f.window.place();
    REQUIRE(place);
    place->rescanPrefabs();
    CHECK(place->prefabFiles().size() == 2);
    CHECK(place->pendingPrefabIcons() == 2);
    CHECK(place->renderPrefabIcons() == 2);
    CHECK(place->pendingPrefabIcons() == 0);
    QListWidget* list = place->prefabList();
    REQUIRE(list->count() == 2);
    for (int i = 0; i < list->count(); ++i) {
        const QString file = list->item(i)->data(Qt::UserRole).toString();
        if (file.endsWith(QStringLiteral("crate.map.json"))) {
            CHECK_FALSE(list->item(i)->icon().isNull());
            CHECK(list->item(i)->toolTip().contains(QStringLiteral("2 x 2 x 2 m")));
        } else {
            CHECK(list->item(i)->icon().isNull());
            CHECK(list->item(i)->toolTip().contains(QStringLiteral("No picture")));
        }
    }
}
