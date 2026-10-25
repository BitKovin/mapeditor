#pragma once
#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/playable.h"
#include <functional>
#include <memory>
#include <vector>

namespace core {

// Which surfaces can the player see: rays from viewpoints spread through the playable area to sample points of a polygon, against the light-blocking
// geometry. Used to send what nobody can see (inside far closed buildings, the backs of far walls) to the last quality cascade (PlayableLod).
class PlayableVisibility {
public:
    // `blocks`: which surfaces block the view (null = every surface of a light-blocking class, which is how it used to be). Pass the static world's opaque
    // surfaces: glass, water and cutouts are see-through, and a door or a trigger volume is not a wall.
    PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t maxViewpoints = 96,
                       std::function<bool(const LightSurface&)> blocks = {});
    ~PlayableVisibility();
    size_t viewpointCount() const;
    // True when some viewpoint sees some sample point of the convex polygon (`normal`: its unit normal).
    bool seen(const std::vector<Vec3>& polygon, const Vec3& normal) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace core
