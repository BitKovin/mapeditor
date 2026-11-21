// tools/terrain -- terrain (port of shell/src/model/terrain.ts, DESIGN 8.k, REQUIREMENTS 39). A terrain is ONE func_detail entity holding a grid of closed "slab" brushes, so the
// Core's validation, lightmapping and rBSP export see ordinary brushes. The entity's JSON carries a record `terrain = {cols, rows, cell, tile}`, every tile brush carries
// `terrainTile = [tx, ty]`. There is no second copy of the heights: they ARE the y of the top vertices of the tiles, read back by `terrainOf`.
//
// A tile covers up to `tile` x `tile` cells and is a closed manifold of planar faces: the (w+1)(d+1) top grid points then the 2(w+d) points of the bottom ring; 2 triangles per cell
// (the diagonal alternates like a chessboard), one quad wall per cell edge on the tile border, one bottom polygon; walls and bottom are common/caulk, the top keeps the painted
// texture; every edge between two top triangles is smooth. Sculpting only replaces a tile's vertices (face ids and the triangulation never change).
// Thread: UI thread (stroke); the pure functions any thread.
// Spec: 07_EDITOR_PORT.md B (terrain.ts), 08_MILESTONES.md M11 item 8.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc/document.h"
#include "doc/edit.h"
#include "scene/point_box.h"

namespace native::tools::terrain {

using Json = nlohmann::json;
using scene::V3;

struct Spec {
    int cols = 0, rows = 0;
    double cell = 1;
    int tile = 16;
    bool operator==(const Spec& o) const { return cols == o.cols && rows == o.rows && cell == o.cell && tile == o.tile; }
};
enum class Tool { Raise, Lower, Smooth, Flatten, Paint };

constexpr const char* kSideTexture = "common/caulk";   // walls and underside: not drawn by the game
constexpr double kMinThickness = 0.25;                 // the top never goes closer than this to the bottom plane
constexpr int kMaxCellsPerSide = 512, kMaxCells = 65536;
constexpr double kRaisePerStrength = 0.5;              // metres per stamp at strength 1

/// The heights of a terrain and where it sits: grid point (gx, gz) is at (ox + gx cell, h[gz (cols + 1) + gx], oz + gz cell); the bottom plane is y = base.
struct Heightfield {
    Spec spec;
    double ox = 0, oz = 0, base = 0;
    std::vector<double> h;
    double at(int gx, int gz) const { return h[static_cast<size_t>(gz) * static_cast<size_t>(spec.cols + 1) + static_cast<size_t>(gx)]; }
};

int tilesAcross(const Spec& s);
int tilesDown(const Spec& s);
struct TileCells {
    int x0 = 0, z0 = 0, w = 0, d = 0;
};
TileCells tileCells(const Spec& s, int tx, int ty);

/// The record of an entity's JSON (null when absent or damaged).
std::optional<Spec> specOf(const Json& rawEntity);
Json specJson(const Spec& s);
/// The tile a brush's JSON names.
std::optional<std::array<int, 2>> tileOf(const Json& rawBrush);
/// A short plain-language summary for the panel.
std::string describeSpec(const Spec& s);

/// The terrain an entity holds, or null when it has no (valid) record or its tiles no longer are what the record says (cached per entity node).
std::shared_ptr<const Heightfield> terrainOf(const std::shared_ptr<const doc::EntityNode>& e);
/// The texture of the top faces of cell (gi, gj) (empty when unknown).
std::string cellTexture(const doc::EntityNode& e, const Spec& spec, int gi, int gj);

// ---- creating ----
struct CreateParams {
    double sizeX = 64, sizeZ = 64;   // metres
    double cell = 1;                 // metres
    int tile = 16;                   // cells per tile side
    V3 origin{0, 0, 0};              // the corner with the smallest x and z, at the height of the flat ground
    double thickness = 8;            // slab depth below the ground
    std::string texture = "dev/grey";
};
/// The grid a CreateParams describes, or why it cannot be made (`error`).
std::optional<Spec> gridFor(const CreateParams& p, std::string& error);
/// A flat terrain as a func_detail entity `entityId` with its tile brushes (ids from `newBrushId`), the record and tile marks set. False with the editor's error / the grid error.
bool addTerrain(doc::VersionEditor& ed, const CreateParams& p, const std::string& entityId, const std::function<std::string()>& newBrushId, std::string& error);

/// Keeps records honest after an edit (`prev` -> `next`, as `reconcileTerrain` of the Shell): a terrain stays one while every tile that changed still is exactly what the record
/// says; otherwise the record and the tile marks are dropped (ordinary brushes), and tile marks without a record are dropped too. Returns `next` itself when nothing had to change.
std::shared_ptr<const doc::DocumentVersion> reconcileTerrain(const std::shared_ptr<const doc::DocumentVersion>& prev, const std::shared_ptr<const doc::DocumentVersion>& next);

// ---- sampling ----
/// Height of the surface at (x, z) exactly as the triangles of the tiles have it (outside the grid: the nearest edge).
double heightAt(const Heightfield& hf, double x, double z);
struct RayHit {
    double t = 0;
    V3 point{0, 0, 0};
};
/// The nearest point where a ray meets the surface (top triangles only), walking the cells the ray crosses.
std::optional<RayHit> rayTerrain(const Heightfield& hf, const V3& origin, const V3& dir);
/// Line segments (flat x y z x y z ...) of a circle of radius r laid on the ground at (cx, cz), `lift` above it.
std::vector<double> circleOnTerrain(const Heightfield& hf, double cx, double cz, double r, int segments = 64, double lift = 0.05);

// ---- brushes ----
struct BrushParams {
    double radius = 6, strength = 0.3, falloff = 0.7;
};
double brushWeight(double d, double radius, double falloff);
struct PointRect {
    int gx0 = 0, gx1 = 0, gz0 = 0, gz1 = 0;
    bool operator==(const PointRect& o) const { return gx0 == o.gx0 && gx1 == o.gx1 && gz0 == o.gz0 && gz1 == o.gz1; }
};
struct StampOptions {
    bool invert = false;
    std::optional<double> target;
};
/// One stamp of a height brush at (cx, cz); changes `hf.h` in place and returns the grid points touched (nullopt when none). Not for Paint.
std::optional<PointRect> stamp(Heightfield& hf, Tool tool, double cx, double cz, const BrushParams& p, const StampOptions& opts = {});
/// Tile indices (ty * tilesAcross + tx) whose brush holds any grid point of the rect (a point on a tile border belongs to both tiles).
std::vector<int> tilesOfRect(const Spec& s, const PointRect& r);
/// Cells (gj * cols + gi) whose centre is within `radius` of (cx, cz).
std::vector<int> cellsInRadius(const Heightfield& hf, double cx, double cz, double radius);

struct StrokeOptions {
    bool invert = false;                       // Shift: Raise <-> Lower, may change during the stroke
    std::optional<double> flattenHeight;       // Flatten: a fixed height (else the height under the first stamp)
    std::string texture;                       // Paint
};
/// One drag of a brush over a terrain: a working copy of the heights, stamps along the path at a fixed spacing (so the result does not depend on how many pointer events came),
/// and `document(start)`, which returns `start` with the tiles changed so far. Only the tiles a stamp touched are rebuilt.
class Stroke {
public:
    Stroke(const std::string& entityId, std::shared_ptr<const Heightfield> hf, Tool tool, const BrushParams& params, const StrokeOptions& opts = {});
    const std::string& entityId() const { return entityId_; }
    const Heightfield& heights() const { return hf_; }
    Tool tool() const { return tool_; }
    bool touched() const { return !heightDirty_.empty() || !paintDirty_.empty() || !done_.empty(); }
    void setInvert(bool invert) { invert_ = invert; }
    void stampAt(double x, double z);
    /// The brush moved to (x, z): stamps along the way, every `spacing` metres; the first call stamps where it stands.
    void moveTo(double x, double z);
    /// Stamps again where the brush stands (holding still keeps working).
    void repeat();
    /// `start` with every tile the stroke has changed so far; `start` itself while nothing changed. Calls with the same `start` build on the previous result.
    std::shared_ptr<const doc::DocumentVersion> document(const std::shared_ptr<const doc::DocumentVersion>& start);

private:
    double spacing() const;

    std::string entityId_;
    Heightfield hf_;
    Tool tool_;
    BrushParams params_;
    StrokeOptions opts_;
    bool invert_ = false;
    std::optional<double> target_;
    std::optional<std::array<double, 2>> last_;
    double carry_ = 0;
    std::set<int> heightDirty_, paintDirty_, done_;
    std::map<int, std::string> painted_;   // cell -> texture
    std::shared_ptr<const doc::DocumentVersion> lastStart_, lastResult_;
};

} // namespace native::tools::terrain
