#pragma once
// How a face's texture is laid out, as the editor draws it: repetition behaviour (mirror, anchor, alternate texture, edge clamp), texture group layouts
// (rule groups and simple texture sets) and texture reliefs (REQUIREMENTS §19 A4-A6, §20, §27 G2 / G3). A C++ port of the Shell's pure functions
//   shell/src/model/texprops.ts, seamless.ts, uv.ts (projection), uvmesh.ts, rng.ts, texgroup.ts, tileset.ts, layout.ts, layoutPlan.ts, relief.ts, reliefApply.ts
// so the BSP export (also from the command line, without the editor) produces exactly the geometry the editor shows. The two implementations are kept
// equal by shared test vectors (testdata/texplan_vectors.json, written by the Shell's tests, checked by the Core's).
#include "core/brush.h"
#include "core/math3.h"
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {
struct GameProfile;
}

namespace core::texplan {

using V2 = std::array<double, 2>;
using V3 = std::array<double, 3>;
inline V3 v3(const Vec3& v) { return {v.x, v.y, v.z}; }
inline Vec3 vec(const V3& v) { return {v[0], v[1], v[2]}; }

// ---- texture properties (texprops.ts) ---------------------------------------------------------------------------------------------------------------
enum class AxisMode { Repeat, Mirror, Anchor };
enum class Fill { Repeat, Mirror };
enum class AnchorSide { Start, End };
enum class EdgeMode { Auto, Wrap, Clamp };
struct AltTexture { std::string texture; V2 offset{0, 0}; };
struct AxisRepeat {
    AxisMode mode = AxisMode::Repeat;
    AnchorSide anchor = AnchorSide::Start;
    double split = 0.5;
    Fill fill = Fill::Mirror;
    std::optional<AltTexture> alt;
    EdgeMode edge = EdgeMode::Auto;
};
struct Relief {
    std::vector<V3> vertices;              // x, y in 0..1 over one repeat (y up), z metres along the normal
    std::vector<std::vector<int>> faces;   // CCW seen from +z
};
struct TextureProps {
    std::optional<double> pixelsPerMeter;
    AxisRepeat u, v;                       // u anchor default start, v anchor default end
    std::shared_ptr<const Relief> relief;  // null = flat
};
TextureProps defaultProps();
/// Lenient like the Shell: anything unusable keeps the default. `self` = the texture's own path (it cannot be its own alternate).
TextureProps parseProps(const nlohmann::json& j, const std::string& self = "");
std::shared_ptr<const Relief> parseRelief(const nlohmann::json& j);
bool isFlatRelief(const Relief& r);
bool isPlainProps(const TextureProps& p);
bool safeTexturePath(const std::string& p);

struct TexInfo {
    std::string path;
    int width = 0, height = 0;
    double ppm = 64;                                  // effective (the profile's default applied)
    TextureProps props;
    std::optional<std::array<bool, 2>> seamless;      // per axis (u, v): do the opposite edges continue each other? unknown = treated as seamless
};
using InfoFn = std::function<const TexInfo*(const std::string& path)>;
bool edgeClamp(const TexInfo& i, int axis);
inline V2 naturalSize(const TexInfo& i) { return {i.width / i.ppm, i.height / i.ppm}; }

/// seamless.ts: mean absolute RGB difference of two RGBA lines, and whether the first / last lines continue each other.
double lineDifference(const uint8_t* a, const uint8_t* b, size_t pixels);
/// Edge analysis of an RGBA image (rows top to bottom), as the Shell's analyseSeams: per axis (u = columns, v = rows).
std::array<bool, 2> analyseSeams(const uint8_t* rgba, int width, int height);

struct FaceRepeatAxis { std::optional<AxisMode> mode; std::optional<EdgeMode> edge; };
struct FaceRepeat { std::optional<FaceRepeatAxis> u, v; };
std::optional<FaceRepeat> parseFaceRepeat(const nlohmann::json& j);
TexInfo withFaceRepeat(const TexInfo& info, const std::optional<FaceRepeat>& over);

// ---- projection and the face's UV map (uv.ts, uvmesh.ts) --------------------------------------------------------------------------------------------
enum class ProjKind { Axial, Ortho, Cam, Free };
struct Projection {
    V2 scale{2, 2}, shift{0, 0};
    double rotate = 0, shear = 0;
    ProjKind kind = ProjKind::Axial;
    std::optional<std::array<V3, 2>> basis;
};
/// The face's `projection` (raw JSON, may be null) with the defaults of its texture (`info` of the face's texture, may be null).
Projection projectionOf(const nlohmann::json* raw, const TexInfo* info);
V2 projectPoint(const V3& n, const V3& q, const Projection& p);

using Aff = std::array<double, 4>;  // a x + b y + c z + d
struct UvAffine { Aff u{}, v{}; };
struct Extent { double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300; };
inline double evalAff(const Aff& a, const V3& p) { return a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3]; }
UvAffine projectionAffine(const V3& n, const Projection& p);
std::optional<UvAffine> affineFromCorners(const std::vector<V3>& pts, const std::vector<V2>& uvs, const V3& n, double tol = 1e-3);
Extent extentOf(const UvAffine& m, const std::vector<V3>& pts);
V3 faceNormalOf(const Brush& b, const BrushFace& f);
/// The face's raw `projection` / `repeat` / `layout` (BrushFace::look), or null.
const nlohmann::json* faceKey(const BrushFace& f, const char* key);
UvAffine faceAffine(const Brush& b, const BrushFace& f, const InfoFn& info);
/// faceUvAt: the UV the face is drawn with at any point of its plane (stored affine uvs, else per triangle of stored uvs, else the projection).
std::function<V2(const V3&)> faceUvAt(const Brush& b, const BrushFace& f, const InfoFn& info);
/// geometry.ts triangulateFace: ear clipping in the face's plane, winding kept.
std::vector<std::array<uint32_t, 3>> triangulateFace(const Brush& b, const BrushFace& f);

// ---- one axis --------------------------------------------------------------------------------------------------------------------------------------
struct Seg { double lo, hi, s, c; bool alt = false; std::optional<int> tile; };
struct AltRatio { double k, off; };
double edgeInsetTexels(double size);
/// nullopt when more than MAX_SEGS segments would be needed.
std::optional<std::vector<Seg>> axisSegments(const AxisRepeat& cfg, const AltRatio* alt, double lo, double hi, bool tilesMain = false, bool tilesAlt = false);
inline constexpr int kMaxCells = 4096;

// ---- a face plan --------------------------------------------------------------------------------------------------------------------------------------
struct PieceVert { V3 w; V2 uv; };
struct Piece { std::string texture; std::vector<PieceVert> verts; };
struct FacePlan {
    std::string texture;
    std::vector<std::string> textures;
    std::function<std::vector<Piece>(const std::array<V3, 3>& tri)> pieces;
};
using PlanPtr = std::shared_ptr<const FacePlan>;
PlanPtr makePlan(const TexInfo& A, const InfoFn& info, const UvAffine& map, const Extent& ext);

// ---- texture groups (texgroup.ts, tileset.ts, layout.ts, layoutPlan.ts) -----------------------------------------------------------------------------
uint32_t hash32(const std::string& s);
double rand01(uint32_t seed, const std::string& key);
uint32_t faceSeed(const std::string& brushId, uint32_t faceId);

struct TexGroup;  // opaque: parsed rule group or texture set
using GroupPtr = std::shared_ptr<const TexGroup>;
struct GroupParse { GroupPtr group; std::vector<std::string> errors, warnings; };
/// Group files are read keeping their key order (slots, rules: the first slot is the default fallback), as JavaScript does.
GroupParse parseGroup(const nlohmann::ordered_json& j);
std::vector<std::string> groupTextures(const TexGroup& g);
std::optional<std::string> groupFallbackTexture(const TexGroup& g);

struct Cell {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string texture, slot;
    std::string fit = "tile";     // tile | stretch | width | height
    std::string origin = "face";  // face | cell
    std::string alignU = "start", alignV = "start";
    bool flipU = false, flipV = false;
    int rotate = 0;
    std::optional<double> cropU, cropV;
    std::string path;
};
struct ParamValue {
    enum class T { Num, Bool, Str } t = T::Num;
    double n = 0;
    bool b = false;
    std::string s;
};
struct LayoutInput {
    const TexGroup* group = nullptr;
    std::map<std::string, ParamValue> params;
    double w = 0, h = 0;
    std::string facing = "wall";
    double tilt = 90;
    uint32_t seed = 0;
    std::function<double(double, double)> worldY;
    InfoFn info;
    std::string fallbackTexture;
    bool hasFallbackTexture = false;
};
struct LayoutResult { std::vector<Cell> cells; std::vector<std::string> warnings; bool truncated = false; };
LayoutResult solveLayout(const LayoutInput& in);

struct FaceLayout {
    std::string group;
    std::optional<uint32_t> seed;
    std::map<std::string, ParamValue> params;
    int rotate = 0;
    bool flipU = false;
};
std::optional<FaceLayout> parseFaceLayout(const nlohmann::json& j);

struct Frame { V3 ex, ey; double dx = 0, dy = 0, w = 0, h = 0; V3 normal; V3 q; double xq = 0, yq = 0; };
std::optional<Frame> layoutFrame(const Brush& b, const BrushFace& f, int rotate = 0, bool flipU = false);
using Aff2 = std::array<double, 3>;
struct CellMap { Aff2 u, v; };
CellMap cellMap(const Cell& c, const V2& tex, double fw, double fh);
PlanPtr buildLayoutPlan(const Frame& frame, const std::vector<Cell>& cells, const std::string& fallback, const InfoFn& info);
using GroupFn = std::function<GroupPtr(const std::string& name)>;
PlanPtr layoutPlanFor(const Brush& b, const BrushFace& f, const FaceLayout& layout, const InfoFn& info, const GroupFn& groups);

/// planForFace: the plan of an authored face (layout first, else repetition), null = plain.
PlanPtr planForFace(const Brush& b, const BrushFace& f, const InfoFn& info, const GroupFn& groups);

// ---- relief (reliefApply.ts) -----------------------------------------------------------------------------------------------------------------------
struct TexturedTri { std::string texture; std::array<V3, 3> pos; std::array<V2, 3> uv; std::array<V3, 3> normal; };
inline constexpr int kMaxReliefCells = 4096;
/// The height of a relief along a line: from parameter t0 to t1 of the line it runs linearly from z0 to z1.
struct ReliefSpan { double t0, t1, z0, z1; };
/// The face next to an outline edge of a face, when it stands at a convex corner with it and shows a relief: its outward normal and its relief's heights (along
/// that normal) along the edge, over t = 0..1 from the edge's first end to its second. `spans` only cover intervals where the neighbour has actual relief
/// surface; a missing interval is not a zero-height surface and must not be bridged to the underlying brush wall. Where both relief surfaces exist, the wall
/// that closes the outline runs between them (a bevel across the corner) instead of down to the surface.
struct Neighbour { V3 n; std::vector<ReliefSpan> spans; };
using NeighbourFn = std::function<std::optional<Neighbour>(const V3& a, const V3& b)>;
/// What stands next to the outline edges of `face` of `brush`: the other face with the edge that lies behind the face's plane and shows a relief of its own laid
/// out by its UV map alone (a face with repeat behaviour keeps the wall down to the surface).
NeighbourFn neighbourFn(const Brush& brush, const BrushFace& face, const InfoFn& info, const GroupFn& groups);
struct ReliefBudget { long long left = 300000; };
/// The relief over one triangle: whole triangles (position, normal, uv), or nullopt when the triangle stays flat (degenerate, too many repeats, budget).
/// `border`: bit k set = the triangle's edge k (corner k to k + 1) is on the outline of the face, so the relief is closed there by a wall down to the surface;
/// walls also close the gaps between repeats whose borders have different heights.
/// `neighbour` tells what stands next to such an outline edge (see Neighbour).
std::optional<std::vector<TexturedTri>> reliefTriangle(const std::array<V3, 3>& tri, const std::array<V2, 3>& uv, const V3& n, const Relief& relief,
                                                       const std::string& texture, ReliefBudget* budget, int border = 0, const NeighbourFn* neighbour = nullptr);
/// For the polygons that make up one face (the fragments a cut leaves of it): which edge of which polygon (edge k: vertex k to k + 1) is on the outline of
/// their union (1) and which is shared with another of them (0). An edge a neighbour only partly shares counts as outline.
std::vector<std::vector<char>> polygonBorders(const std::vector<std::vector<V3>>& polys);
/// The outline flags (polygonBorders) of the polygons of many authored faces, worked out one face at a time when first asked: `add` every polygon with the
/// key of the face it lies on (and its texture), then ask `of`. An edge is an outline only when no other face continues the surface across it: a coplanar
/// polygon of the same texture that runs the edge the other way (a wall built from several brushes has no outline at the joints between them).
class OutlineIndex {
public:
    size_t add(const std::string& faceKey, std::vector<V3> poly, const std::string& texture = std::string());
    const std::vector<char>& of(size_t polygon);
private:
    bool continued(size_t polygon, const V3& a, const V3& b);
    void buildGrid();
    std::vector<std::vector<V3>> polys_;
    std::vector<std::string> keys_, textures_;
    std::vector<V3> normals_;
    std::unordered_map<long long, std::vector<size_t>> grid_;  // 4 m cells -> polygons whose bounds touch them
    std::vector<size_t> big_;                                  // polygons too large for the grid: always candidates
    bool gridBuilt_ = false;
    std::map<std::string, std::vector<size_t>> groups_;
    std::map<size_t, std::vector<char>> done_;
};
/// The border bits of the fan triangle (0, i, i + 1) of a polygon with n corners, given which of its edges are on the outline.
int fanBorder(size_t i, size_t n, const std::vector<char>& edge);
/// The border bits of a piece of a source triangle (`w` = its corners' weights in it).
int pieceBorder(const std::vector<PieceVert>& verts, int triBorder);
/// The relief of a texture when it changes anything.
std::shared_ptr<const Relief> reliefOf(const InfoFn& info, const std::string& texture);

/// Everything the editor draws for one convex polygon of a face (a whole face, a carved fragment): textured triangles in the face's plane or on its
/// relief. `parentBrush` / `parentFace` = the authored face it lies on (null: plain, default projection); `normal` = the polygon's own (outward) normal;
/// `border[k]` = edge k of `poly` is on the outline of the face (see polygonBorders; null: none is).
std::vector<TexturedTri> texturePolygon(const std::vector<V3>& poly, const V3& normal, const Brush* parentBrush, const BrushFace* parentFace, const InfoFn& info,
                                        const GroupFn& groups, ReliefBudget* budget, const std::string& defaultTexture = "dev/grey",
                                        const std::vector<char>* border = nullptr);

// ---- the environment: textures and groups from the assets folder -----------------------------------------------------------------------------------
class TexEnvironment {
public:
    TexEnvironment(std::filesystem::path assetsRoot, const GameProfile& profile);
    ~TexEnvironment();
    /// null while the texture cannot be found or decoded (the face is then drawn plainly, like the Shell before its image loaded).
    const TexInfo* info(const std::string& path);
    GroupPtr group(const std::string& name);
    InfoFn infoFn() { return [this](const std::string& p) { return info(p); }; }
    GroupFn groupFn() { return [this](const std::string& n) { return group(n); }; }
    /// Lets tests (and callers that already know the textures) provide the infos directly.
    void setInfo(const TexInfo& i);
    void setGroup(const std::string& name, GroupPtr g);
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace core::texplan
