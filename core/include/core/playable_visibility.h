#pragma once
#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/playable.h"
#include "core/bsp_vis.h"
#include <functional>
#include <memory>
#include <vector>

namespace core {

// Which surfaces can the player see: the baked visibility of the export (bsp_vis.h) built for the playable zones. A surface is seen when part of it lies outside
// the cells (everything beyond the playable zones is always visible) or in a cluster that a cluster inside a zone sees. Used to send what nobody can see
// (closed rooms and the backs of walls near the zones) to the last quality cascade (PlayableLod).
class PlayableVisibility {
public:
    // `blocks`: which surfaces block the view (null = every surface of a light-blocking class). Pass the static world's opaque surfaces: glass, water and
    // cutouts are see-through, and a door or a trigger volume is not a wall. `solids`: the volumes that block the view (null = none known).
    // `maxViewpoints` is not used any more.
    PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t maxViewpoints = 96,
                       std::function<bool(const LightSurface&)> blocks = {}, const SolidSet* solids = nullptr);
    // The visibility of the export, built once (buildBspVisibility).
    explicit PlayableVisibility(std::shared_ptr<const VisResult> shared);
    ~PlayableVisibility();
    size_t viewpointCount() const;  // the clusters
    // True when the convex polygon is seen (`normal`: its unit normal).
    bool seen(const std::vector<Vec3>& polygon, const Vec3& normal) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace core
