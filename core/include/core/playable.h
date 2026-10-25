#pragma once
#include "core/document.h"
#include "core/math3.h"
#include "core/prefab.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace core {

// The playable area (editor-only volumes, entity class `editor_playable_area`, entity_class.h): where the player can be. Lighting spends its
// budget there: lightmap density falls off in cascades with the distance from the area, and the light volume only covers the area (plus a margin).
// An empty area (no volume in the level) means "everywhere": nothing is reduced.
struct PlayableLod {
    double radius = 16;         // m: surfaces up to this far from the area keep the full density (cascade 0)
    int levels = 4;             // cascades beyond the full-density one; surfaces further out stay in the last one
    double range = 2;           // each cascade reaches `range` times further than the one before: cascade k ends at radius * range^k
    double density = 0.5;       // lightmap texel density multiplier per cascade (cascade k: density^k)
    double lightvolMargin = 2;  // m: the light volume covers the area grown by this
    bool operator==(const PlayableLod& o) const {
        return radius == o.radius && levels == o.levels && range == o.range && density == o.density && lightvolMargin == o.lightvolMargin;
    }
    // cascade of a surface `d` metres from the area: 0 = full density
    int cascade(double d) const;
    // density multiplier for a surface `d` metres from the area
    double densityAt(double d) const;
};

class PlayableArea {
public:
    struct Piece {
        Vec3 mn, mx;
        std::vector<std::pair<Vec3, double>> planes;  // outward unit normal, offset: n . x <= d inside
    };
    bool empty() const { return pieces_.empty(); }
    size_t pieceCount() const { return pieces_.size(); }
    void add(const Brush& b);
    void finish();
    Vec3 mn() const { return mn_; }  // bounds of all the volumes
    Vec3 mx() const { return mx_; }
    // Distance (m) from `p` to the area: 0 inside any volume. A lower bound of the true distance (it errs towards more detail).
    // `maxDist`: the result never exceeds it (lets the search skip pieces that cannot be closer).
    double distance(const Vec3& p, double maxDist = 1e300) const;
    // Distance from a convex polygon (a surface piece) to the area: the smallest distance over sample points of the polygon (vertices, edges,
    // interior at about 2 m spacing); 0 when any is inside.
    double distance(const std::vector<Vec3>& polygon, double maxDist = 1e300) const;
    // True when `p` is within `margin` of the area.
    bool near(const Vec3& p, double margin) const { return distance(p, margin + 1.0) <= margin; }
    uint64_t fingerprint() const { return fp_; }

private:
    std::vector<Piece> pieces_;
    Vec3 mn_{1e300, 1e300, 1e300}, mx_{-1e300, -1e300, -1e300};
    uint64_t fp_ = 0;
};

/// Every volume of a playable-area entity in the document and its prefabs (convex pieces of each brush; subtractive brushes are ignored).
std::shared_ptr<const PlayableArea> collectPlayableArea(const Document& doc, const std::vector<ResolvedEntity>& prefabs);

}  // namespace core
