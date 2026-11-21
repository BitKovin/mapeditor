// T11.4d: the editing model of simple texture sets (tools/texturing/tileset_edit): reading and writing the "kit", the first guess from names, and the changes the editor makes.
#include <doctest.h>

#include <core/texplan.h>

#include "tools/texturing/tileset_edit.h"

using namespace native::tools::tex;
using json = nlohmann::json;

TEST_CASE("kit: a wall set round-trips through its file form; defaults are not written") {
    const json src = json::parse(R"({"kind": "wall", "tile": [3.2, 3.2], "avoidRepeat": false,
        "bottom": {"pieces": [{"tex": "bb/door", "place": "once"}, "bb/plain"]},
        "middle": {"rows": 2, "pieces": [{"cells": [["bb/win_a", "bb/win_b"], ["bb/win_c", "bb/win_d"]], "weight": 0.3, "match": "win 1"}]},
        "top": {"pieces": [{"tex": "bb/cap"}]}})");
    std::vector<std::string> w;
    const auto k = kitFromJson(src, w);
    REQUIRE(k);
    CHECK(w.empty());
    CHECK(k->kind == KitKind::Wall);
    REQUIRE(k->tile);
    CHECK((*k->tile)[0] == doctest::Approx(3.2));
    CHECK_FALSE(k->avoidRepeat);
    CHECK(k->lane(Lane::Bottom).pieces.size() == 2);
    CHECK(k->lane(Lane::Bottom).pieces[0].place == Place::Once);
    CHECK(k->lane(Lane::Middle).rows == 2);
    const KitPiece& win = k->lane(Lane::Middle).pieces[0];
    CHECK(win.cols() == 2);
    CHECK(win.rows() == 2);
    CHECK(win.weight == doctest::Approx(0.3));
    CHECK(win.match == "win 1");
    const nlohmann::ordered_json out = kitToJson(*k);
    CHECK(out["kind"] == "wall");
    CHECK(out["bottom"]["pieces"][1]["tex"] == "bb/plain");   // a 1 x 1 piece is compact
    CHECK_FALSE(out["bottom"]["pieces"][1].contains("weight"));
    CHECK(out["middle"]["rows"] == 2);
    CHECK(out["avoidRepeat"] == false);
    CHECK_FALSE(out.contains("maxStretch"));
    std::vector<std::string> w2;
    const auto again = kitFromJson(json::parse(out.dump()), w2);
    REQUIRE(again);
    CHECK(kitToJson(*again).dump() == out.dump());
    CHECK(kitTextures(*k) == std::vector<std::string>{"bb/cap", "bb/win_a", "bb/win_b", "bb/win_c", "bb/win_d", "bb/door", "bb/plain"});   // lane order: top, middle, bottom
}

TEST_CASE("kit: a frame has nine slots; bad paths and kinds are reported, pieces of another height are left out") {
    std::vector<std::string> w;
    const auto f = kitFromJson(json::parse(R"({"kind": "frame", "maxStretch": 50, "tl": "a/tl", "c": "a/c", "br": "../evil", "t": null})"), w);
    REQUIRE(f);
    CHECK(f->kind == KitKind::Frame);
    CHECK(f->maxStretch == doctest::Approx(0.5));
    CHECK(f->frame[0] == "a/tl");
    CHECK(f->frame[4] == "a/c");
    CHECK_FALSE(f->frame[8]);
    CHECK(w.size() == 1);
    const auto out = kitToJson(*f);
    CHECK(out["maxStretch"] == 50);
    CHECK(out["t"].is_null());
    CHECK(out["tl"] == "a/tl");
    std::vector<std::string> w2;
    CHECK_FALSE(kitFromJson(json::parse(R"({"kind": "tower"})"), w2));
    CHECK_FALSE(w2.empty());
    CHECK_FALSE(kitFromJson(json::parse("[]"), w2));
    std::vector<std::string> w3;
    const auto m = kitFromJson(json::parse(R"({"kind": "wall", "middle": {"rows": 2, "pieces": [{"tex": "a/one"}, {"cells": [["a/x"], ["a/y"]]}]}})"), w3);
    REQUIRE(m);
    CHECK(m->lane(Lane::Middle).pieces.size() == 1);   // the one-row piece does not fit a two-row lane
    CHECK_FALSE(w3.empty());
}

TEST_CASE("kit: the file Core reads is the file the editor writes") {
    Kit k = emptyWallKit();
    addPiece(k, Lane::Middle, std::string("dev/wall"), Place::Any, kWeightOften);
    addPiece(k, Lane::Bottom, std::string("dev/door"), Place::Once, 1);
    addPiece(k, Lane::Top, std::string("dev/cap"));
    const auto file = tileSetFile(k, "Test set", "");
    const auto parse = core::texplan::parseGroup(file);
    CHECK(parse.errors.empty());
    CHECK(parse.group);
    CHECK(file["name"] == "Test set");
    CHECK_FALSE(file.contains("description"));
    Kit f = emptyFrameKit();
    for (int i = 0; i < 9; ++i) f.frame[static_cast<size_t>(i)] = std::string("dev/grey");
    const auto fp = core::texplan::parseGroup(tileSetFile(f, "", ""));
    CHECK(fp.errors.empty());
    CHECK(fp.group);
}

TEST_CASE("suggest: names sort textures into lanes, doors go once to the bottom, styles become match names") {
    const Kit k = suggestWallKit({"bb/roof_cap", "bb/brick_wall", "bb/ground_floor", "bb/door_1", "bb/window_2", "bb/base_plain"});
    CHECK(k.lane(Lane::Top).pieces.size() == 1);
    REQUIRE(k.lane(Lane::Middle).pieces.size() == 2);
    CHECK(k.lane(Lane::Middle).pieces[0].weight == doctest::Approx(3));
    CHECK(k.lane(Lane::Middle).pieces[1].match == "window 2");
    CHECK(k.lane(Lane::Middle).pieces[1].weight == doctest::Approx(1));
    REQUIRE(k.lane(Lane::Bottom).pieces.size() == 3);
    CHECK(k.lane(Lane::Bottom).pieces[1].place == Place::Once);
    CHECK(k.lane(Lane::Bottom).pieces[1].match == "door 1");
    // a bottom lane with only a door gets the plain middle pieces to fill with
    const Kit d = suggestWallKit({"bb/door", "bb/plain"});
    CHECK(d.lane(Lane::Bottom).pieces.size() == 2);
    CHECK(d.lane(Lane::Bottom).pieces[1].place == Place::Any);
}

TEST_CASE("changes: lane rows resize pieces, pieces move, duplicate, delete and take cells; the limits hold") {
    Kit k = emptyWallKit();
    const int a = addPiece(k, Lane::Middle, std::string("a/one"));
    const int b = addPiece(k, Lane::Middle, std::string("a/two"));
    REQUIRE(a == 0);
    REQUIRE(b == 1);
    setLaneRows(k, Lane::Middle, 2);
    CHECK(k.lane(Lane::Middle).pieces[0].rows() == 2);
    CHECK(k.lane(Lane::Middle).pieces[0].cells[1][0] == "a/one");   // the last row repeats
    setLaneRows(k, Lane::Middle, 99);
    CHECK(k.lane(Lane::Middle).rows == kMaxPiece);
    setLaneRows(k, Lane::Middle, 1);
    fitPieceCols(k.lane(Lane::Middle).pieces[0], 3);
    CHECK(k.lane(Lane::Middle).pieces[0].cols() == 3);
    CHECK(setPieceCell(k, Lane::Middle, 0, 0, 2, std::string("a/side")));
    CHECK(k.lane(Lane::Middle).pieces[0].cells[0][2] == "a/side");
    CHECK_FALSE(setPieceCell(k, Lane::Middle, 0, 5, 0, std::nullopt));
    CHECK_FALSE(setPieceCell(k, Lane::Middle, 9, 0, 0, std::nullopt));
    CHECK(movePiece(k, Lane::Middle, 1, Lane::Bottom) == 0);
    CHECK(k.lane(Lane::Middle).pieces.size() == 1);
    CHECK(k.lane(Lane::Bottom).pieces.size() == 1);
    CHECK(movePiece(k, Lane::Middle, 5, Lane::Top) == -1);
    CHECK(movePiece(k, Lane::Middle, 0, Lane::Middle) == 0);
    CHECK(duplicatePiece(k, Lane::Middle, 0) == 1);
    CHECK(k.lane(Lane::Middle).pieces.size() == 2);
    CHECK(duplicatePiece(k, Lane::Middle, 7) == -1);
    removePiece(k, Lane::Middle, 0);
    CHECK(k.lane(Lane::Middle).pieces.size() == 1);
    removePiece(k, Lane::Middle, 9);   // out of range: nothing
    CHECK(k.lane(Lane::Middle).pieces.size() == 1);
    for (int i = 0; i < kMaxPieces + 3; ++i) addPiece(k, Lane::Top, std::string("a/t") + std::to_string(i));
    CHECK(k.lane(Lane::Top).pieces.size() == static_cast<size_t>(kMaxPieces));
    CHECK(addPiece(k, Lane::Top, std::string("a/more")) == -1);
}

TEST_CASE("addTextures: pieces go to the lanes their names say; an empty lane takes a one row height") {
    Kit k = emptyWallKit();
    setLaneRows(k, Lane::Middle, 2);   // no pieces yet
    CHECK(addTextures(k, {"x/roof", "x/wall", "x/window_1"}) == 3);
    CHECK(k.lane(Lane::Top).pieces.size() == 1);
    CHECK(k.lane(Lane::Middle).pieces.size() == 2);
    CHECK(k.lane(Lane::Middle).rows == 1);
    CHECK(k.lane(Lane::Middle).pieces[0].rows() == 1);
}
