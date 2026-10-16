#include "core/brush.h"
#include "core/document.h"
#include "test_util.h"
#include <fstream>
#include <doctest.h>

using namespace core;
using namespace testutil;

static bool contains(const std::optional<std::string>& s, const char* needle) {
    return s && s->find(needle) != std::string::npos;
}

TEST_CASE("makeBox produces a structurally valid, closed, outward-wound box") {
    Brush b = Brush::makeBox("b", {-1, -1, -1}, {1, 2, 3});
    CHECK(b.vertices.size() == 8);
    CHECK(b.faces.size() == 6);
    CHECK_FALSE(b.checkStructure().has_value());
    CHECK_FALSE(b.checkConvexSolid().has_value());
    CHECK(b.signedVolume() == doctest::Approx(2.0 * 3.0 * 4.0));

    const Vec3 centre{0, 0.5, 1};
    for (const auto& f : b.faces) {
        Vec3 faceCentre;
        for (uint32_t idx : f.loop) faceCentre = faceCentre + b.vertices[idx] * (1.0 / f.loop.size());
        CHECK(b.faceNormal(f).dot(faceCentre - centre) > 0);  // normal points away from the solid
    }
}

TEST_CASE("bounds and translate") {
    Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 2, 3});
    b.translate({10, -5, 0.5});
    Vec3 mn, mx;
    REQUIRE(b.bounds(mn, mx));
    CHECK(mn.x == doctest::Approx(10));
    CHECK(mn.y == doctest::Approx(-5));
    CHECK(mx.z == doctest::Approx(3.5));
    CHECK(b.signedVolume() == doctest::Approx(6.0));  // translation doesn't change volume
}

TEST_CASE("convex non-box solids are accepted; concave ones are rejected with a clear reason") {
    Brush triPrism = makePrism("tri", {{0, 0}, {2, 0}, {0, 2}}, 0, 1);
    CHECK_FALSE(triPrism.checkConvexSolid().has_value());

    Brush l = lPrism("l");
    CHECK_FALSE(l.checkStructure().has_value());  // a fine mesh...
    auto err = l.checkConvexSolid();              // ...just not a convex one
    REQUIRE(err.has_value());
    CHECK(contains(err, "convex"));
}

TEST_CASE("inside-out, open and degenerate meshes are rejected as solids") {
    SUBCASE("inside-out") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        for (auto& f : b.faces) std::reverse(f.loop.begin(), f.loop.end());
        CHECK(contains(b.checkConvexSolid(), "inward"));
    }
    SUBCASE("open (a face missing)") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces.pop_back();
        CHECK(contains(b.checkConvexSolid(), "not closed"));
    }
    SUBCASE("flat") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 0, 1});  // zero height
        CHECK(b.checkConvexSolid().has_value());
    }
    SUBCASE("non-planar face") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.vertices[6].y += 0.5;  // drags three faces out of plane
        CHECK(b.checkConvexSolid().has_value());
    }
}

TEST_CASE("structural validation catches malformed indexed meshes") {
    SUBCASE("index out of range") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces[0].loop[0] = 99;
        CHECK(contains(b.checkStructure(), "references vertex 99"));
    }
    SUBCASE("loop too short") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces[0].loop.resize(2);
        CHECK(contains(b.checkStructure(), "fewer than 3"));
    }
    SUBCASE("repeated vertex in loop") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces[0].loop[1] = b.faces[0].loop[0];
        CHECK(contains(b.checkStructure(), "twice"));
    }
    SUBCASE("uv count mismatch") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces[0].uvs = {{0, 0}};
        CHECK(contains(b.checkStructure(), "uvs"));
    }
    SUBCASE("duplicate face id") {
        Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
        b.faces[1].id = b.faces[0].id;
        CHECK(contains(b.checkStructure(), "duplicate face id"));
    }
    SUBCASE("no faces") {
        Brush b;
        CHECK(contains(b.checkStructure(), "no faces"));
    }
}

TEST_CASE("shared brush vectors (also run by the Shell's test suite)") {
    std::ifstream in(std::string(TESTDATA_DIR) + "/brush_vectors.json");
    REQUIRE(in);
    const json vectors = json::parse(in);
    for (const auto& c : vectors["cases"]) {
        INFO(c["name"].get<std::string>());
        const Brush b = brushFromJson(c["brush"]);
        auto err = b.checkStructure();
        if (!err && c["requireConvex"].get<bool>()) err = b.checkConvexSolid();
        if (c["expect"].is_null()) {
            CHECK_FALSE(err.has_value());
        } else {
            REQUIRE(err.has_value());
            CHECK(err->find(c["expect"].get<std::string>()) != std::string::npos);
        }
    }
}

TEST_CASE("non-planar faces are rejected for every brush, not just convex-required ones") {
    Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
    b.vertices[6].y += 0.5;
    CHECK(contains(b.checkStructure(), "not planar"));
}
