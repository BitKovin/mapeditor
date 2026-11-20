// T4.5: the editor preferences, the game profile form and the status bar texts (tools/prefs, tools/profile_form, tools/status_text).
#include <doctest.h>

#include <core/profile.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/prefs.h"
#include "tools/profile_form.h"
#include "tools/status_text.h"

using namespace native;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() / ("native_settings_test_" + std::to_string(++n) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::shared_ptr<const doc::DocumentVersion> mapWith(std::initializer_list<const char*> classes) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    int i = 0;
    for (const char* c : classes) {
        core::Entity e;
        e.id = "e" + std::to_string(i);
        e.classname = c;
        if (std::string(c) == "worldspawn") {
            e.brushes.push_back(core::Brush::makeBox("b1", {0, 0, 0}, {1, 1, 1}, "dev/grey"));
            e.brushes.push_back(core::Brush::makeBox("b2", {2, 0, 0}, {3, 1, 1}, "dev/grey"));
            e.brushes.push_back(core::Brush::makeBox("b3", {4, 0, 0}, {5, 1, 1}, "dev/grey"));
        }
        REQUIRE(ed.addEntity(e));
        ++i;
    }
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

} // namespace

TEST_CASE("editor prefs: defaults, clamping, JSON round trip and applying to a viewport") {
    const tools::EditorPrefs d;
    CHECK(tools::prefsFromJson("") == d);
    CHECK(tools::prefsFromJson("not json") == d);
    CHECK(tools::prefsFromJson("[1,2]") == d);

    tools::EditorPrefs p;
    p.gridSize = 2;
    p.snapEnabled = false;
    p.rotSnap = 45;
    p.scaleSnap = 0.25;
    p.flySpeed = 20;
    p.showGrid = false;
    CHECK(tools::prefsFromJson(tools::prefsToJson(p)) == p);

    // bad values are clamped or ignored, good ones next to them are kept
    const tools::EditorPrefs c = tools::prefsFromJson(R"({"gridSize": -3, "flySpeed": 100000, "rotSnap": "x", "snapEnabled": false, "showGrid": 7})");
    CHECK(c.gridSize == d.gridSize);
    CHECK(c.flySpeed == render::kFlySpeedMax);
    CHECK(c.rotSnap == d.rotSnap);
    CHECK_FALSE(c.snapEnabled);
    CHECK(c.showGrid == d.showGrid);

    tools::EditorPrefs bad;
    bad.gridSize = std::nan("");
    bad.scaleSnap = 0;
    bad.flySpeed = 0.0001;
    const tools::EditorPrefs s = tools::sanitize(bad);
    CHECK(s.gridSize == d.gridSize);
    CHECK(s.scaleSnap == d.scaleSnap);
    CHECK(s.flySpeed == render::kFlySpeedMin);

    render::ViewportSettings vs;
    vs.cameraMode = render::CameraMode::Top;
    vs.renderMode = render::RenderMode::Lit;
    vs.showOutlines = true;
    const render::ViewportSettings out = tools::applyPrefs(p, vs);
    CHECK(out.gridSize == 2);
    CHECK_FALSE(out.snapEnabled);
    CHECK(out.rotSnap == 45);
    CHECK(out.scaleSnap == 0.25);
    CHECK(out.flySpeed == 20);
    CHECK_FALSE(out.showGrid);
    CHECK(out.cameraMode == render::CameraMode::Top);   // what is not a preference stays
    CHECK(out.renderMode == render::RenderMode::Lit);
    CHECK(out.showOutlines);
    CHECK(tools::prefsOf(out) == p);
}

TEST_CASE("profile form: lists, paths and the changes to write") {
    CHECK(tools::splitList("a, b;c\nd  e") == std::vector<std::string>{"a", "b", "c", "d", "e"});
    CHECK(tools::splitList("").empty());
    CHECK(tools::splitExtensions("tga, .jpg png") == std::vector<std::string>{".tga", ".jpg", ".png"});
    CHECK(tools::joinList({"x", "y"}, ", ") == "x, y");

    CHECK(tools::checkRelativePath("textures", "T").empty());
    CHECK(tools::checkRelativePath("a/b/c.ent", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("../x", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("a/../../x", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("/abs", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("\\abs", "T").empty());
    CHECK_FALSE(tools::checkRelativePath("C:/abs", "T").empty());

    TempDir none;
    tools::ProfileForm a = tools::loadProfileForm(none.path).form;   // the built-in defaults
    a.name = "g";
    tools::ProfileForm b = a;
    CHECK(tools::profileChanges(a, b).empty());
    b.mapRoot = "levels";
    b.textureExtensions = "tga png";
    b.pixelsPerMeter = 128;
    b.blockLight = "func_door\nfunc_wall";
    const nlohmann::json ch = tools::profileChanges(a, b);
    CHECK(ch.size() == 4);
    CHECK(ch["mapRoot"] == "levels");
    CHECK(ch["textureExtensions"] == nlohmann::json::array({".tga", ".png"}));
    CHECK(ch["pixelsPerMeter"] == 128);
    CHECK(ch["lighting"]["blockLight"] == nlohmann::json::array({"func_door", "func_wall"}));
    CHECK_FALSE(ch["lighting"].contains("receiveBakedLight"));
    // the same list written another way is no change
    tools::ProfileForm c = a;
    c.textureExtensions = "tga,jpg,jpeg,png";   // the default list, written another way
    CHECK(tools::profileChanges(a, c).empty());

    CHECK(tools::validateProfileForm(a).empty());
    tools::ProfileForm bad = a;
    bad.mapRoot = "../x";
    bad.pixelsPerMeter = 0;
    bad.name = "";
    bad.textureExtensions = " ";
    bad.exporter = "zip";
    CHECK(tools::validateProfileForm(bad).size() == 5);
}

TEST_CASE("profile form: load, save and keep what the form does not know") {
    TempDir dir;
    // no file: the defaults, nothing exists
    tools::LoadedForm none = tools::loadProfileForm(dir.path);
    CHECK_FALSE(none.fileExists);
    CHECK(none.error.empty());
    CHECK(none.form.textureRoot == "textures");
    CHECK(none.form.exporter == "rbsp");

    // a file with a key the form knows nothing of
    writeText(dir.path / "game_profile.json",
              R"({"name": "mine", "mapRoot": "levels", "lighting": {"blockLight": ["func_door"], "lightStyles": {"12": "mmmmaaaa"}}, "bsp": {"contentFlags": {"solid": 1}}})");
    tools::LoadedForm lf = tools::loadProfileForm(dir.path);
    CHECK(lf.fileExists);
    CHECK(lf.form.name == "mine");
    CHECK(lf.form.mapRoot == "levels");
    CHECK(lf.form.blockLight == "func_door");

    tools::ProfileForm edited = lf.form;
    edited.prefabRoot = "prefabs/new";
    edited.blockLight = "func_door\nfunc_wall";
    edited.exporter = "obj";
    CHECK(tools::saveProfileForm(dir.path, lf.form, edited).empty());
    const nlohmann::json file = core::readProfileFile(dir.path);
    CHECK(file["prefabRoot"] == "prefabs/new");
    CHECK(file["exporter"] == "obj");
    CHECK(file["lighting"]["blockLight"] == nlohmann::json::array({"func_door", "func_wall"}));
    CHECK(file["lighting"]["lightStyles"]["12"] == "mmmmaaaa");   // kept
    CHECK(file["bsp"]["contentFlags"]["solid"] == 1);               // kept
    CHECK(core::loadProfile(dir.path).prefabRoot == "prefabs/new");

    // a form that cannot be valid is refused and the file is not touched
    const std::string before = readText(dir.path / "game_profile.json");
    tools::ProfileForm broken = edited;
    broken.mapRoot = "../../elsewhere";
    CHECK_FALSE(tools::saveProfileForm(dir.path, edited, broken).empty());
    CHECK(readText(dir.path / "game_profile.json") == before);

    // nothing changed: nothing written (the file may not even exist)
    TempDir empty;
    CHECK(tools::saveProfileForm(empty.path, none.form, none.form).empty());
    CHECK_FALSE(fs::exists(empty.path / "game_profile.json"));
    // the first change creates the file
    tools::ProfileForm first = none.form;
    first.name = "new game";
    CHECK(tools::saveProfileForm(empty.path, none.form, first).empty());
    CHECK(core::loadProfile(empty.path).name == "new game");

    // a malformed file is reported and the form falls back to the defaults
    writeText(dir.path / "game_profile.json", "{ not json");
    const tools::LoadedForm lb = tools::loadProfileForm(dir.path);
    CHECK_FALSE(lb.error.empty());
    CHECK(lb.form.textureRoot == "textures");
}

TEST_CASE("status texts") {
    CHECK(tools::shortNumber(0.5) == "0.5");
    CHECK(tools::shortNumber(2) == "2");
    CHECK(tools::shortNumber(22.5) == "22.5");
    CHECK(tools::shortNumber(0.125) == "0.125");
    CHECK(tools::shortNumber(std::nan("")) == "0");

    render::ViewportSettings s;
    s.gridSize = 0.5;
    s.rotSnap = 15;
    CHECK(tools::gridText(s) == "Grid 0.5 m, snap on, rot 15\xC2\xB0");
    s.snapEnabled = false;
    s.gridSize = 4;
    CHECK(tools::gridText(s) == "Grid 4 m, snap off");

    CHECK(tools::folderText("") == "No game folder");
    CHECK(tools::folderText("F:/Git/GameData") == "GameData");
    CHECK(tools::folderText("F:/Git/GameData/") == "GameData");
    CHECK(tools::folderText("F:\\Git\\GameData") == "GameData");
    CHECK(tools::folderText("data") == "data");

    CHECK(tools::mapText(nullptr, false) == "No map");
    const auto d = mapWith({"worldspawn", "light_point", "light_point"});
    CHECK(tools::mapText(d.get(), false) == "untitled");
    CHECK(tools::mapText(d.get(), true) == "untitled *");

    doc::Selection sel;
    CHECK(tools::selectionText(sel, d.get()) == "Nothing selected");
    sel.select(doc::SelRef::entity("e1"));
    CHECK(tools::selectionText(sel, d.get()) == "light_point");
    CHECK(tools::selectionText(sel, nullptr) == "1 entity");
    sel.select(doc::SelRef::brush("e0", "b1"));
    CHECK(tools::selectionText(sel, d.get()) == "1 brush");
    sel.toggle(*d, doc::SelRef::brush("e0", "b2"));
    CHECK(tools::selectionText(sel, d.get()) == "2 brushes");
    sel.toggle(*d, doc::SelRef::entity("e1"));
    sel.toggle(*d, doc::SelRef::entity("e2"));
    CHECK(tools::selectionText(sel, d.get()) == "2 entities, 2 brushes");
    sel.select(doc::SelRef::face("e0", "b1", 3));
    CHECK(tools::selectionText(sel, d.get()) == "1 face");
    sel.select(doc::SelRef::instance("i1"));
    CHECK(tools::selectionText(sel, d.get()) == "1 prefab instance");
}
