// tools/texturing/tileset_edit -- the editing model of simple texture sets (port of the data part of shell/src/model/tileset.ts, REQUIREMENTS section 20 "simple sets"): a wall set is three
// lanes (Top, Middle, Bottom) of pieces, a piece being one texture or a block of textures that stay together; a frame is nine textures (9-slice). The set is stored in a group file
// under "kit"; Core reads it (core::texplan::parseGroup) and lays it out, this file only reads and writes the JSON and changes it for the editor window.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (tileSetEditor), 08_MILESTONES.md M11 item 4.
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace native::tools::tex {

enum class Place { Any, Left, Right, Middle, Once };
const char* placeName(Place p);             // "any", "left", "right", "middle", "once"
Place placeFromName(const std::string& s);   // unknown = Any
enum class Lane { Top = 0, Middle = 1, Bottom = 2 };
inline constexpr Lane kLanes[3] = {Lane::Top, Lane::Middle, Lane::Bottom};
const char* laneName(Lane l);               // "top", "middle", "bottom"
inline constexpr int kMaxPiece = 6, kMaxPieces = 48;
inline constexpr double kDefaultMaxStretch = 0.25;
inline constexpr double kWeightRare = 0.3, kWeightSometimes = 1, kWeightOften = 3;

using Cells = std::vector<std::vector<std::optional<std::string>>>;   // rows top to bottom; nullopt = not chosen yet (the piece is not used)
struct KitPiece {
    Cells cells;
    double weight = 1;
    Place place = Place::Any;
    std::optional<std::string> match;   // pieces with different names never sit directly on each other
    int cols() const { return cells.empty() ? 0 : static_cast<int>(cells[0].size()); }
    int rows() const { return static_cast<int>(cells.size()); }
};
struct KitLane {
    int rows = 1;
    std::vector<KitPiece> pieces;
};
inline constexpr const char* kFrameSlots[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
enum class KitKind { Wall, Frame };
struct Kit {
    KitKind kind = KitKind::Wall;
    std::optional<std::array<double, 2>> tile;   // metres; nullopt = the size of the first texture
    double maxStretch = kDefaultMaxStretch;      // a fraction (0.25 = 25 %), 0 = no limit
    bool avoidRepeat = true;
    KitLane lanes[3];                             // wall: Top, Middle, Bottom
    std::array<std::optional<std::string>, 9> frame;   // frame: kFrameSlots order
    KitLane& lane(Lane l) { return lanes[static_cast<int>(l)]; }
    const KitLane& lane(Lane l) const { return lanes[static_cast<int>(l)]; }
};
Kit emptyWallKit();
Kit emptyFrameKit();
/// Every texture the set uses, once, in order.
std::vector<std::string> kitTextures(const Kit& k);

/// Reads the "kit" of a group file. Lenient like Core: unusable parts are dropped and reported in `warnings`; nullopt when it is no kit at all (then `warnings` says why).
std::optional<Kit> kitFromJson(const nlohmann::json& j, std::vector<std::string>& warnings);
/// The stored form (compact: a 1 x 1 piece is { "tex": ... }, defaults are left out).
nlohmann::ordered_json kitToJson(const Kit& k);
/// A whole group file: { "format": 1, "name"?, "description"?, "kit": ... }.
nlohmann::ordered_json tileSetFile(const Kit& k, const std::string& title, const std::string& description);

/// Sorts textures into lanes by what their names say (roof / cap / cornice -> Top; bottom / base / ground / plinth -> Bottom; a door -> Bottom, placed once; windows Sometimes,
/// plain walls Often). Only a first guess.
Kit suggestWallKit(const std::vector<std::string>& paths);

// ---- changes the editor makes -------------------------------------------------------------------------------------------------------------
void fitPieceRows(KitPiece& p, int rows);   // a taller piece repeats its last row, a shorter one keeps its top rows
void fitPieceCols(KitPiece& p, int cols);
void setLaneRows(Kit& k, Lane lane, int rows);   // every piece of the lane is resized
/// Adds a piece; its index, or -1 when the lane is full.
int addPiece(Kit& k, Lane lane, const std::optional<std::string>& tex, Place place = Place::Any, double weight = 1);
void removePiece(Kit& k, Lane lane, int index);
int duplicatePiece(Kit& k, Lane lane, int index);   // the copy's index, or -1
int movePiece(Kit& k, Lane from, int index, Lane to);   // the index in the new lane, or -1
/// Adds textures with suggestWallKit's guess of the lane; returns how many pieces were added.
int addTextures(Kit& k, const std::vector<std::string>& paths);
bool setPieceCell(Kit& k, Lane lane, int index, int row, int col, const std::optional<std::string>& tex);

} // namespace native::tools::tex
