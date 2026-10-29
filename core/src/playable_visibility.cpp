#include "core/playable_visibility.h"
#include "core/bsp_vis.h"

namespace core {

// The same baked visibility the exported map carries (bsp_vis.h), built for the playable zones.
struct PlayableVisibility::Impl {
    std::shared_ptr<const VisResult> result;
};

PlayableVisibility::PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t,
                                       std::function<bool(const LightSurface&)> blocks, const SolidSet* solids)
    : d_(new Impl) {
    std::vector<LightSurface> occ;
    for (const LightSurface& s : surfaces) {
        const bool blocking = blocks ? blocks(s) : classes.castsShadows(s.classname);
        if (!blocking) continue;
        occ.push_back(s);
        occ.back().relief.reset();
    }
    VisInput in;
    in.occluders = &occ;
    in.solids = solids;
    for (const auto& pc : area.pieces()) in.zones.push_back({pc.mn, pc.mx, pc.planes});
    d_->result = std::make_shared<const VisResult>(buildVisibility(in));
}

PlayableVisibility::PlayableVisibility(std::shared_ptr<const VisResult> shared) : d_(new Impl) { d_->result = std::move(shared); }

PlayableVisibility::~PlayableVisibility() = default;

size_t PlayableVisibility::viewpointCount() const { return size_t(d_->result->clusters); }

bool PlayableVisibility::seen(const std::vector<Vec3>& poly, const Vec3&) const { return d_->result->seen(poly); }

}  // namespace core
