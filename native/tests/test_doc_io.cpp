// T2.2: the document model and file I/O: every shipped map round-trips byte for byte, unknown keys survive, errors are reported,
// saving is atomic, lvl2 loads fast. Headless, no GPU.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "base/log.h"
#include "base/timing.h"
#include "doc/document.h"
#include "doc/io.h"

namespace fs = std::filesystem;
using namespace native;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path coreAssets() { return fs::path(NATIVE_CORE_ASSETS_DIR); }
const fs::path kLvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";

// A scratch folder removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("native_doc_io_" + std::to_string(doc::nextVersionNumber()) + "_" +
                                            std::to_string(static_cast<uint64_t>(base::nowMicros())));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Every map and prefab file of the Core's assets (the .map.json files only).
std::vector<fs::path> shippedDocuments() {
    std::vector<fs::path> out;
    for (const char* sub : {"maps", "prefabs"}) {
        const fs::path dir = coreAssets() / sub;
        if (!fs::exists(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string name = e.path().filename().string();
            if (e.is_regular_file() && name.size() > 9 && name.compare(name.size() - 9, 9, ".map.json") == 0) out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A small document built from the first brush of the shipped demo map (so it is valid), with editor-only keys the Core never
// models at the document, entity, brush and face level, an entity with an empty "brushes" array and no prefabInstances key.
std::string tinyDocText() {
    const nlohmann::json demo = nlohmann::json::parse(readFile(coreAssets() / "maps" / "demo.map.json"));
    nlohmann::json brush;
    for (const auto& e : demo.at("entities"))
        if (e.contains("brushes") && !e["brushes"].empty()) {
            brush = e["brushes"][0];
            break;
        }
    brush["editorColor"] = "#ff0000";
    brush["faces"][0]["weirdFaceKey"] = 7;
    nlohmann::json world = {{"id", "world"}, {"classname", "worldspawn"}, {"editorOnly", {{"visgroup", "walls"}}}, {"brushes", {brush}}};
    nlohmann::json start = {{"id", "e_start"}, {"classname", "info_player_start"}, {"origin", {1, 2, 3}}, {"brushes", nlohmann::json::array()}};
    nlohmann::json doc = {{"schemaVersion", "0.2"}, {"customRootKey", {{"keep", {1, 2, 3}}}}, {"entities", {world, start}}};
    return doc.dump(2) + "\n";
}

} // namespace

TEST_CASE("a tiny document keeps its unknown keys at every level and round-trips exactly") {
    const std::string canonical = tinyDocText();   // what the editor itself writes
    const doc::LoadResult r = doc::loadDocumentText(canonical, "maps/tiny.map.json");
    INFO(r.error);
    REQUIRE(r.version != nullptr);
    const doc::DocumentVersion& v = *r.version;
    CHECK(v.path == "maps/tiny.map.json");
    CHECK(v.version > 0);
    CHECK(v.entities.size() == 2);
    CHECK(v.brushCount() == 1);
    CHECK(v.rootRaw.contains("customRootKey"));
    CHECK_FALSE(v.rootRaw.contains("entities"));
    CHECK(v.entities[0]->raw.contains("editorOnly"));
    CHECK_FALSE(v.entities[0]->raw.contains("brushes"));        // brushes live in their own nodes
    CHECK(v.entities[0]->hasBrushesKey);
    CHECK(v.entities[1]->hasBrushesKey);                        // an empty "brushes" array is remembered
    CHECK(v.entities[0]->brushes[0]->raw["faces"][0].contains("weirdFaceKey"));
    CHECK(v.entities[0]->brushes[0]->raw.contains("editorColor"));
    CHECK(v.entities[0]->brushes[0]->contentHash != 0);
    CHECK(v.entities[0]->typed.classname == "worldspawn");
    CHECK(v.entities[0]->typed.brushes.empty());
    CHECK(v.entities[0]->brushes[0]->brush.faces.size() >= 4);
    CHECK(v.entities[1]->typed.origin.y == doctest::Approx(2));
    CHECK_FALSE(v.hasInstancesKey);                             // the file had no prefabInstances: none are invented

    CHECK(doc::serializeDocument(v) == canonical);

    // Lookup tables.
    CHECK(v.findEntity("world") == 0);
    CHECK(v.findEntity("e_start") == 1);
    CHECK(v.findEntity("nope") == -1);
    const std::string brushId = v.entities[0]->brushes[0]->brush.id;
    CHECK(v.findBrush("world", brushId) == 0);
    CHECK(v.findBrush("world", "no_such_brush") == -1);
    CHECK(v.findBrush("e_start", brushId) == -1);

    // The typed document assembled from the nodes has the brushes back in their entity.
    const core::Document typed = doc::toCoreDocument(v);
    REQUIRE(typed.entities.size() == 2);
    CHECK(typed.entities[0].brushes.size() == 1);
    CHECK_FALSE(core::validateDocument(typed).has_value());
}

TEST_CASE("versions share their nodes; a copy has its own lookup tables") {
    const doc::LoadResult r = doc::loadDocumentText(tinyDocText());
    REQUIRE(r.version != nullptr);
    doc::DocumentVersion next = *r.version;                     // an edit starts from a copy
    next.version = doc::nextVersionNumber();
    CHECK(next.version > r.version->version);
    CHECK(next.entities[0].get() == r.version->entities[0].get());   // same node objects
    // Replace one node: the other stays shared. (The lookup tables are built on the first lookup, so a version is changed before
    // anything looks things up in it, and published after.)
    auto changed = std::make_shared<doc::EntityNode>(*next.entities[1]);
    changed->typed.id = "e_renamed";
    next.entities[1] = changed;
    CHECK(next.entities[0].get() == r.version->entities[0].get());
    CHECK(r.version->findEntity("e_start") == 1);               // the old version is untouched
    CHECK(r.version->findEntity("e_renamed") == -1);
    CHECK(next.findEntity("e_renamed") == 1);
}

TEST_CASE("loading reports malformed and invalid documents without throwing") {
    CHECK(doc::loadDocumentText("{ not json").error.find("not valid JSON") != std::string::npos);
    CHECK_FALSE(doc::loadDocumentText("[]").error.empty());
    CHECK_FALSE(doc::loadDocumentText(R"({"entities": []})").error.empty());                       // no schemaVersion
    CHECK(doc::loadDocumentText(R"({"schemaVersion": "9.9"})").error.find("unsupported schemaVersion") != std::string::npos);
    CHECK_FALSE(doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": [3]})").error.empty());
    CHECK_FALSE(doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": [{"classname": "x"}]})").error.empty());   // no id
    CHECK_FALSE(doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": {}})").error.empty());
    // Two entities with one id: syntactically fine, rejected by validateDocument.
    const auto dup = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": [{"id": "a", "classname": "x"}, {"id": "a", "classname": "y"}]})");
    CHECK(dup.version == nullptr);
    CHECK(dup.error.find("invalid document") != std::string::npos);
    // A minimal document loads.
    const auto ok = doc::loadDocumentText(R"({"schemaVersion": "0.2"})");
    CHECK(ok.error.empty());
    CHECK(ok.version->entities.empty());
    CHECK(doc::loadDocumentFile("this/file/does/not/exist.map.json").error.find("cannot open") != std::string::npos);
}

TEST_CASE("saving is atomic and creates folders; a failed save leaves the old file") {
    TempDir dir;
    const doc::LoadResult r = doc::loadDocumentText(tinyDocText());
    REQUIRE(r.version != nullptr);
    const fs::path file = dir.path / "maps" / "sub" / "tiny.map.json";
    CHECK(doc::saveDocumentFile(*r.version, file).empty());
    CHECK(readFile(file) == doc::serializeDocument(*r.version));
    CHECK_FALSE(fs::exists(fs::path(file.string() + ".tmp")));
    // Saving again replaces the file.
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "old";
    CHECK(doc::saveDocumentFile(*r.version, file).empty());
    CHECK(readFile(file) == doc::serializeDocument(*r.version));
    // The destination is a folder: the save fails with a message and nothing is left behind.
    const fs::path blocked = dir.path / "blocked.map.json";
    fs::create_directories(blocked);
    CHECK_FALSE(doc::saveDocumentFile(*r.version, blocked).empty());
    CHECK(fs::is_directory(blocked));
    CHECK_FALSE(fs::exists(fs::path(blocked.string() + ".tmp")));
}

TEST_CASE("every shipped map and prefab loads and saves back without losing anything") {
    // The shipped files were written by generator scripts (key order, indentation, line endings of their own), so a byte comparison
    // with the original is meaningless for them; lvl2 (written by the editor) is compared byte for byte below. What must hold:
    //  - the saved JSON is the same data as the original (nothing lost, nothing added);
    //  - the saved bytes are the Core's format: dump(2) + "\n" of the parsed original;
    //  - loading the saved file and saving again changes nothing (idempotent).
    const std::vector<fs::path> files = shippedDocuments();
    REQUIRE_MESSAGE(files.size() >= 4, "no maps found under ", coreAssets().string());
    TempDir dir;
    int identical = 0;
    for (const fs::path& file : files) {
        INFO("file " << file.filename().string());
        const std::string original = readFile(file);
        const nlohmann::json originalJson = nlohmann::json::parse(original);
        const doc::LoadResult r = doc::loadDocumentFile(file, file.filename().string());
        REQUIRE_MESSAGE(r.version != nullptr, r.error);
        const std::string saved = doc::serializeDocument(*r.version);
        CHECK(nlohmann::json::parse(saved) == originalJson);
        CHECK(saved == originalJson.dump(2) + "\n");
        if (saved == original) ++identical;

        const fs::path out = dir.path / file.filename();
        REQUIRE(doc::saveDocumentFile(*r.version, out).empty());
        CHECK(readFile(out) == saved);
        const doc::LoadResult again = doc::loadDocumentFile(out);
        REQUIRE_MESSAGE(again.version != nullptr, again.error);
        CHECK(doc::serializeDocument(*again.version) == saved);
        CHECK(again.brushes == r.brushes);
    }
    MESSAGE(identical << " of " << files.size() << " shipped files are already byte-identical to the editor's format");
}

TEST_CASE("lvl2 loads in under 3 seconds and round-trips") {
    if (!fs::exists(kLvl2)) {
        MESSAGE("lvl2 not found at ", kLvl2.string(), ": skipped");
        return;
    }
    const std::string original = readFile(kLvl2);
    const doc::LoadResult r = doc::loadDocumentFile(kLvl2, "maps/lvl2.map.json");
    REQUIRE_MESSAGE(r.version != nullptr, r.error);
    MESSAGE("lvl2: " << r.bytes / 1024 / 1024 << " MB, " << r.version->entities.size() << " entities, " << r.brushes << " brushes; read "
                     << r.timings.readMs << " ms, parse " << r.timings.parseMs << " ms, nodes " << r.timings.nodesMs << " ms, validate "
                     << r.timings.validateMs << " ms, total " << r.timings.totalMs << " ms");
    native::base::logInfo("doc: lvl2 loaded in " + std::to_string(r.timings.totalMs) + " ms");
    CHECK(r.timings.totalMs < 3000);
    base::Stopwatch sw;
    const std::string saved = doc::serializeDocument(*r.version);
    MESSAGE("lvl2 serialize " << sw.elapsedMs() << " ms");
    size_t at = 0;
    while (at < saved.size() && at < original.size() && saved[at] == original[at]) ++at;
    INFO("sizes " << original.size() << " vs " << saved.size() << ", first difference at byte " << at);
    CHECK(saved == original);
}
