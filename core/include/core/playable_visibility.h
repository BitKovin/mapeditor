#pragma once
#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/playable.h"
#include "core/bsp_vis.h"
#include <functional>
#include <memory>
#include <vector>

namespace core {

// Which surfaces can the player see. Inside the cells of the baked visibility (bsp_vis.h; `pvs`) a surface is seen when it lies in a cluster that a cluster inside
// a zone sees. Beyond the cells (the background) the baked visibility says "always visible", so there the answer is ray traced: rays from viewpoints spread
// through the playable area to sample points of the polygon, against the light-blocking geometry. Used to send what nobody can see (inside far closed buildings,
// the backs of far walls) to the last quality cascade, or to remove it (PlayableLod).
class PlayableVisibility {
public:
    // `blocks`: which surfaces block the view (null = every surface of a light-blocking class, which is how it used to be). Pass the static world's opaque
    // surfaces: glass, water and cutouts are see-through, and a door or a trigger volume is not a wall. `pvs`: the baked visibility of the level (null = rays only).
    PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t maxViewpoints = 96,
                       std::function<bool(const LightSurface&)> blocks = {}, std::shared_ptr<const VisResult> pvs = nullptr);
    ~PlayableVisibility();
    size_t viewpointCount() const;
    // True when the convex polygon is seen (`normal`: its unit normal).
    bool seen(const std::vector<Vec3>& polygon, const Vec3& normal) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace core
