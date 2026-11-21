// T11.4a: the editing model of the Texture Properties window (ports of the behaviour of shell/src/model/texprops.ts as far as the sidecar JSON goes). Pure.
#include <doctest.h>

#include "tools/texturing/props_edit.h"

using namespace native::tools::tex;
using json = nlohmann::json;

TEST_CASE("props: pixels per metre is set, clamped and cleared") {
    json p = json::object();
    CHECK_FALSE(ppmOf(p));
    p = withPpm(p, 128);
    CHECK(ppmOf(p) == doctest::Approx(128));
    CHECK(ppmOf(withPpm(p, 100000)) == doctest::Approx(kPpmMax));
    CHECK(ppmOf(withPpm(p, 0.1)) == doctest::Approx(kPpmMin));
    CHECK_FALSE(ppmOf(withPpm(p, std::nullopt)));
    CHECK_FALSE(ppmOf(withPpm(p, -5)));
}

TEST_CASE("props: an axis round-trips; defaults are not written; u anchors at the start and v at the end by default") {
    json p = json::object();
    CHECK(axisOf(p, 0).anchor == "start");
    CHECK(axisOf(p, 1).anchor == "end");
    CHECK(axisOf(p, 0) == defaultAxis(0));
    AxisProps a = defaultAxis(1);
    a.mode = "anchor";
    a.split = 0.25;
    a.fill = "repeat";
    a.edge = "clamp";
    a.altTexture = "wall/top";
    a.altOffset = {0.5, -0.25};
    p = withAxis(p, 1, a);
    const AxisProps back = axisOf(normalizedProps(p), 1);
    CHECK(back == a);
    CHECK(axisOf(p, 0) == defaultAxis(0));   // the other axis untouched
    // setting it back to the default removes it
    p = withAxis(p, 1, defaultAxis(1));
    CHECK_FALSE(p["repeat"].contains("v"));
    // the split is clamped by the validation
    AxisProps wild = defaultAxis(0);
    wild.mode = "anchor";
    wild.split = 5;
    CHECK(axisOf(normalizedProps(withAxis(json::object(), 0, wild)), 0).split == doctest::Approx(0.98));
}

TEST_CASE("props: hotspots are clamped into the texture and capped") {
    json p = withHotspots(json::object(), {{0.1, 0.1, 0.5, 0.5}, {0.9, 0.9, 0.5, 0.5}});
    const auto h = hotspotsOf(normalizedProps(p));
    REQUIRE(h.size() == 2);
    CHECK(h[1].x + h[1].w <= 1.0 + 1e-9);
    CHECK(hotspotsOf(withHotspots(p, {})).empty());
    std::vector<Hotspot> many(100, Hotspot{0.1, 0.1, 0.2, 0.2});
    CHECK(hotspotsOf(normalizedProps(withHotspots(json::object(), many))).size() == kMaxHotspots);
}

TEST_CASE("props: transparency keeps its tint and goes away at the defaults") {
    json p = {{"transparency", {{"mode", "cutout"}, {"cutoff", 0.3}, {"tint", 0.5}}}};
    TransparencyProps t = transparencyOf(normalizedProps(p));
    CHECK(t.mode == "cutout");
    CHECK(t.cutoff == doctest::Approx(0.3));
    t.mode = "translucent";
    t.opacity = 0.4;
    const json q = normalizedProps(withTransparency(p, t));
    CHECK(transparencyOf(q).mode == "translucent");
    CHECK(transparencyOf(q).opacity == doctest::Approx(0.4));
    CHECK(q["transparency"]["tint"].get<double>() == doctest::Approx(0.5));
    const json plain = withTransparency(json::object(), TransparencyProps{});
    CHECK_FALSE(plain.contains("transparency"));
}

TEST_CASE("props: isPlain and forWrite") {
    CHECK(isPlain(json::object()));
    CHECK(isPlain(json{{"hotspots", json::array()}, {"repeat", json::object()}}));
    CHECK_FALSE(isPlain(withPpm(json::object(), 32)));
    CHECK_FALSE(isPlain(withHotspots(json::object(), {{0, 0, 0.5, 0.5}})));
    AxisProps a = defaultAxis(0);
    a.mode = "mirror";
    CHECK_FALSE(isPlain(withAxis(json::object(), 0, a)));
    // forWrite carries every editable key so that the stored ones are replaced
    const json w = forWrite(json::object());
    CHECK(w.contains("hotspots"));
    CHECK(w.contains("repeat"));
    CHECK(w.contains("transparency"));
    CHECK_FALSE(w.contains("pixelsPerMeter"));
}
