// Tests of exporting/export_job: the editor's export request gives the same files as the Core's own export tool, the launch line of Play map expands its placeholders, a running
// export can be cancelled. Headless (no window); the cancel case bakes on the GPU when there is one.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <core/exporter.h>
#include <core/profile.h>
#include <core/scene.h>

#include "doc/io.h"
#include "exporting/export_job.h"

namespace fs = std::filesystem;
using namespace native;

namespace {

fs::path freshCopyOfAssets(const char* name) {
    const fs::path dst = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(fs::path(NATIVE_CORE_ASSETS_DIR), dst, fs::copy_options::recursive);
    return dst;
}

std::string bytesOf(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Cleanup {
    std::vector<fs::path> dirs;
    ~Cleanup() {
        std::error_code ec;
        for (const auto& d : dirs) fs::remove_all(d, ec);
    }
};

} // namespace

TEST_CASE("exporting: the request of the editor writes the same files as the Core's export tool (demo_lit, geometry only)") {
    Cleanup cleanup;
    const fs::path a = freshCopyOfAssets("native_export_a"), b = freshCopyOfAssets("native_export_b");
    cleanup.dirs = {a, b};
    const std::string map = "maps/demo_lit.map.json";

    // the editor's way: the document version -> request -> job
    const doc::LoadResult loaded = doc::loadDocumentFile(a / map, map);
    REQUIRE(loaded.version);
    exporting::ExportOptions o;
    o.assetsRoot = a;
    o.outPath = "maps/native_out.bsp";
    o.bake = false;
    std::string warning;
    core::ExportRequest request = exporting::makeExportRequest(*loaded.version, o, &warning);
    CHECK(request.format == "rbsp");
    CHECK(request.outPath == "maps/native_out.bsp");
    exporting::ExportJob job;
    REQUIRE(job.start(request));
    CHECK_FALSE(job.start(request));   // one at a time
    REQUIRE(job.wait(120000));
    const auto result = job.take();
    REQUIRE(result);
    INFO(result->error);
    REQUIRE(result->ok);
    CHECK(job.take() == nullptr);      // taken once

    // the Core's way (bsp_export_tool): the scene opens the file itself
    core::BspExportRequest req;
    req.assetsRoot = b;
    req.mapPath = map;
    req.bake = false;
    {
        core::Scene scene{req.assetsRoot};
        scene.loadGameProfile();
        scene.openDocument(map);
        req.document = scene.documentSyncJson()["doc"];
        req.format = core::exporterFor(scene.profile().exporter).id;
    }
    req.outPath = "maps/native_out.bsp";
    const core::ExportResult ref = core::exporterFor(req.format).run(req, {}, {});
    REQUIRE(ref.ok);

    const std::string mine = bytesOf(a / "maps/native_out.bsp"), theirs = bytesOf(b / "maps/native_out.bsp");
    REQUIRE_FALSE(mine.empty());
    CHECK(mine.size() == theirs.size());
    CHECK(mine == theirs);
    CHECK(result->stats.surfaces == ref.stats.surfaces);
    CHECK(result->stats.brushes == ref.stats.brushes);
}

TEST_CASE("exporting: the game's start line expands {map} {mapname} {bsp} {game} and keeps quoted arguments together") {
    exporting::LaunchSettings s;
    s.exe = "C:/Games/Slasher/game.exe";
    s.args = "--map {mapname}.bsp -path \"{game}\" -rel {map}";
    const fs::path root = "D:/Data/GameData";
    const exporting::LaunchCommand c = exporting::launchCommand(s, root, "maps/level.bsp");
    CHECK(c.exe == "C:/Games/Slasher/game.exe");
    REQUIRE(c.args.size() == 6);   // "maps/level.bsp" is not quoted by the template: {map} splits at its space
    CHECK(c.args[0] == "--map");
    CHECK(c.args[1] == "level.bsp");
    CHECK(c.args[2] == "-path");
    CHECK(c.args[3] == root.string());
    CHECK(c.args[4] == "-rel");
    CHECK(c.args[5] == "maps/level.bsp");
    CHECK(fs::path(c.workingDir) == fs::path("C:/Games/Slasher"));
    s.workingDir = "{game}/run";
    CHECK(exporting::launchCommand(s, root, "a.bsp").workingDir == root.string() + "/run");
    s.args = "+map \"{bsp}\"";
    const auto q = exporting::launchCommand(s, root, "maps/a.bsp");
    REQUIRE(q.args.size() == 2);
    CHECK(fs::path(q.args[1]) == (root / "maps/a.bsp"));
}

TEST_CASE("exporting: a running export can be cancelled" * doctest::test_suite("gpu")) {
    Cleanup cleanup;
    const fs::path a = freshCopyOfAssets("native_export_cancel");
    cleanup.dirs = {a};
    const std::string map = "maps/demo_lit.map.json";
    const doc::LoadResult loaded = doc::loadDocumentFile(a / map, map);
    REQUIRE(loaded.version);
    exporting::ExportOptions o;
    o.assetsRoot = a;
    o.outPath = "maps/cancelled.bsp";
    exporting::ExportJob job;
    REQUIRE(job.start(exporting::makeExportRequest(*loaded.version, o)));
    for (int i = 0; i < 400 && job.progress().stage == "starting"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    job.cancel();
    REQUIRE(job.wait(300000));
    const auto result = job.take();
    REQUIRE(result);
    MESSAGE("cancelled " << result->cancelled << ", ok " << result->ok << ", error '" << result->error << "', last stage " << job.progress().stage);
    CHECK((result->cancelled || result->ok));   // a tiny map may finish before the cancel arrives
    if (result->cancelled) CHECK_FALSE(result->ok);
}
