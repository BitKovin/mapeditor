// T11.7b: prefab previews (tools/prefab_preview), the logic of shell/src/state/prefabPreview.ts and the picture maths of render/icons.ts.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/prefab_preview.h"

using namespace native;
namespace fs = std::filesystem;

namespace {

struct Folder {
    fs::path root;
    Folder() {
        root = fs::temp_directory_path() / ("prefab_preview_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Folder() { std::error_code ec; fs::remove_all(root, ec); }
    // A prefab file with a box (min..max) in worldspawn, optionally a light at `lightAt` and an instance of `nested` at `nestedAt`.
    void write(const std::string& name, core::Vec3 lo, core::Vec3 hi, const std::optional<core::Vec3>& lightAt = {}, const std::string& nested = {}, core::Vec3 nestedAt = {}) {
        const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
        REQUIRE(r.version);
        doc::VersionEditor ed(r.version);
        core::Entity w;
        w.id = "w";
        w.classname = "worldspawn";
        if (lo.x < hi.x) w.brushes.push_back(core::Brush::makeBox("a", lo, hi, "dev/grey"));
        REQUIRE(ed.addEntity(w));
        if (lightAt) {
            core::Entity l;
            l.id = "l";
            l.classname = "lamp";
            l.origin = *lightAt;
            REQUIRE(ed.addEntity(l));
        }
        if (!nested.empty()) {
            core::PrefabInstance p;
            p.id = "n";
            p.file = nested;
            p.origin = nestedAt;
            REQUIRE(ed.addInstance(p));
        }
        const auto v = ed.finish();
        REQUIRE(v);
        REQUIRE(doc::saveDocumentFile(*v, root / name).empty());
    }
};

const scene::BoxOf boxes = scene::classBoxes(nullptr);
const tools::ColorOf noColor = [](const std::string&) { return std::optional<std::array<double, 3>>(); };

} // namespace

TEST_CASE("prefab previews: a prefab is read and expanded at the origin, nested ones with it") {
    Folder f;
    f.write("prefabs/crate.map.json", {0, 0, 0}, {2, 1, 1});
    f.write("prefabs/pair.map.json", {10, 0, 0}, {11, 1, 1}, {}, "prefabs/crate.map.json", {4, 0, 0});
    tools::PrefabPreviews previews(f.root);
    const tools::PreviewContent crate = previews.get("prefabs/crate.map.json");
    REQUIRE(crate.entities);
    CHECK(crate.error.empty());
    const auto b = tools::contentBounds(*crate.entities, boxes);
    REQUIRE(b);
    CHECK(b->max[0] == doctest::Approx(2));
    const tools::PreviewContent pair = previews.get("prefabs/pair.map.json");
    REQUIRE(pair.entities);
    const auto pb = tools::contentBounds(*pair.entities, boxes);
    REQUIRE(pb);
    CHECK(pb->min[0] == doctest::Approx(4));    // the nested crate, 4 m along x
    CHECK(pb->max[0] == doctest::Approx(11));   // the box of the pair itself
}

TEST_CASE("prefab previews: a missing or empty prefab says why; clear picks up a changed file") {
    Folder f;
    tools::PrefabPreviews previews(f.root);
    const tools::PreviewContent gone = previews.get("prefabs/none.map.json");
    CHECK_FALSE(gone.entities);
    CHECK_FALSE(gone.error.empty());
    f.write("prefabs/box.map.json", {0, 0, 0}, {1, 1, 1});
    CHECK_FALSE(previews.get("prefabs/none.map.json").entities);   // cached
    f.write("prefabs/box.map.json", {0, 0, 0}, {3, 1, 1});
    const auto first = previews.get("prefabs/box.map.json");
    REQUIRE(first.entities);
    CHECK(tools::contentBounds(*first.entities, boxes)->max[0] == doctest::Approx(3));
    f.write("prefabs/box.map.json", {0, 0, 0}, {5, 1, 1});
    CHECK(tools::contentBounds(*previews.get("prefabs/box.map.json").entities, boxes)->max[0] == doctest::Approx(3));   // still the cached one
    previews.clear();
    CHECK(tools::contentBounds(*previews.get("prefabs/box.map.json").entities, boxes)->max[0] == doctest::Approx(5));
}

TEST_CASE("prefab previews: bounds cover brushes and the boxes of point entities") {
    Folder f;
    f.write("p.map.json", {0, 0, 0}, {2, 2, 2}, core::Vec3(6, 1, 1));
    tools::PrefabPreviews previews(f.root);
    const auto c = previews.get("p.map.json");
    REQUIRE(c.entities);
    const auto b = tools::contentBounds(*c.entities, boxes);
    REQUIRE(b);
    CHECK(b->min[0] == doctest::Approx(0));
    CHECK(b->max[0] > 6);   // the lamp's box reaches past its origin
    CHECK_FALSE(tools::contentBounds({}, boxes));
}

TEST_CASE("prefab previews: a box is drawn as its three visible faces, fitted into the square") {
    Folder f;
    f.write("p.map.json", {0, 0, 0}, {2, 1, 1});
    tools::PrefabPreviews previews(f.root);
    const auto c = previews.get("p.map.json");
    REQUIRE(c.entities);
    const int px = 96;
    const tools::IconModel m = tools::iconOf(*c.entities, boxes, noColor, px, 6);
    REQUIRE(m.polys.size() == 3);
    double lo = 1e9, hi = -1e9, loY = 1e9, hiY = -1e9;
    for (const auto& p : m.polys)
        for (const auto& q : p.points) {
            lo = std::min(lo, q[0]);
            hi = std::max(hi, q[0]);
            loY = std::min(loY, q[1]);
            hiY = std::max(hiY, q[1]);
        }
    CHECK(lo >= 6 - 1e-6);
    CHECK(hi <= px - 6 + 1e-6);
    CHECK(loY >= 6 - 1e-6);
    CHECK(hiY <= px - 6 + 1e-6);
    CHECK(std::max(hi - lo, hiY - loY) == doctest::Approx(px - 12));
    CHECK((lo + hi) / 2 == doctest::Approx(px / 2.0));
    // the three faces are lit differently
    CHECK(m.polys[0].color != m.polys[1].color);
    CHECK(m.polys[1].color != m.polys[2].color);
}

TEST_CASE("prefab previews: a point entity is a box in its class colour; near-black is lifted") {
    Folder f;
    f.write("p.map.json", {0, 0, 0}, {0, 0, 0}, core::Vec3(0, 0, 0));   // no brush: only the lamp
    tools::PrefabPreviews previews(f.root);
    const auto c = previews.get("p.map.json");
    REQUIRE(c.entities);
    const tools::IconModel red = tools::iconOf(*c.entities, boxes, [](const std::string&) { return std::optional<std::array<double, 3>>({1.0, 0.0, 0.0}); }, 64);
    REQUIRE(red.polys.size() == 3);
    for (const auto& p : red.polys) {
        CHECK(p.color[0] > 0.3);
        CHECK(p.color[1] == doctest::Approx(0));
    }
    const tools::IconModel dark = tools::iconOf(*c.entities, boxes, [](const std::string&) { return std::optional<std::array<double, 3>>({0.05, 0.05, 0.05}); }, 64);
    for (const auto& p : dark.polys) CHECK(p.color[0] > 0.2);   // lifted to grey
    const tools::IconModel amber = tools::iconOf(*c.entities, boxes, noColor, 64);
    CHECK(amber.polys[0].color[0] > amber.polys[0].color[2]);   // the default amber
}

TEST_CASE("prefab previews: nothing to show gives an empty picture") {
    CHECK(tools::iconOf({}, boxes, noColor, 64).polys.empty());
}
