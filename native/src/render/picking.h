// render/picking -- picking on the CPU: what is under the cursor, and what lies inside a box drawn on the screen.
//
// Owns: Picker, a bounding-volume tree over the objects of a scene snapshot (brush bounds, point-entity boxes) and the exact ray / triangle test on the
// candidates (two-sided Moller-Trumbore as shell/src/render/picking.ts). A brush is hit on its carved pieces (core::LightSurface of the snapshot: the hole you
// cut is clickable, a click on a cut wall names the SUBTRACTOR's face), a subtractive brush and a brush whose pieces are not known on the authored brush.
// Prefab content is picked as its instance. Point entities are hit by their box (scene/point_box: the class box of the entity definitions, a decal's
// projector volume) turned with their angles; without definitions every class has the 0.5 m default box.
// The tree is rebuilt from the snapshot on setSnapshot() (about a millisecond for lvl2); picking never touches the GPU, so a click answers at once.
// Thread: setSnapshot / pick / boxSelect on one thread (the UI thread); the snapshot is immutable.
// Spec: 05_RENDERER.md section 6, 08_MILESTONES.md T3.5.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "doc/selection.h"
#include "render/camera.h"
#include "render/math3d.h"
#include "scene/point_box.h"
#include "scene/snapshot.h"

namespace native::render {

struct PickOptions {
    bool includeSubtractive = true;   // false: subtractive brush shapes are hidden and catch no clicks
};

struct PickHit {
    std::string instance;                 // core::instanceString of the instance the hit lies in ("" = the document itself)
    std::string entityId;                 // the owner of the brush that was hit (a cut wall: the subtractor's entity); the entity for a point entity
    std::string brushId;                  // "" for a point entity
    std::optional<uint32_t> faceId;       // the authored face (unknown when the brush was hit on its raw mesh)
    double t = 0;                         // distance along the ray (ray direction unit length)
    Vec3 point, normal;                   // the normal faces the viewer
    bool subtractive = false;
    bool pointEntity = false;

    // The thing to select for this hit in object mode: the brush (or point entity); inside a prefab the instance. `face` = the face (Shift-click).
    doc::SelRef toSelRef(bool face) const;
};

// A ray through a point of the screen: `ndcX` / `ndcY` in Vulkan normalised device coordinates (-1..1, y down as the matrices).
Ray rayThrough(const Mat4& viewProj, double ndcX, double ndcY);

class Picker {
public:
    void setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot);
    // The class boxes of the point entities (default: 0.5 m for every class); rebuilds the tree when a snapshot is set.
    void setBoxOf(scene::BoxOf boxOf);
    std::shared_ptr<const scene::SceneSnapshot> snapshot() const { return snap_; }
    size_t objectCount() const { return items_.size(); }

    // The nearest hit along the ray, if any.
    std::optional<PickHit> pick(const Ray& ray, const PickOptions& opts = {}) const;

    // Everything whose bounds lie completely inside the screen rectangle [x0, x1] x [y0, y1] (Vulkan NDC) of `viewProj`: solid brushes, subtractive
    // brushes (when included) and point entities. Brushes of prefab instances give their instance once. Sorted by document order of the keys.
    std::vector<doc::SelRef> boxSelect(const Mat4& viewProj, double x0, double y0, double x1, double y1, const PickOptions& opts = {}) const;

private:
    struct Item {
        scene::ObjectKey key;                 // brush items
        std::string pointEntity;              // point entity items: the entity id
        const scene::ObjectProducts* products = nullptr;
        double mn[3], mx[3];                  // axis-aligned bounds (for a turned point entity: around its box)
        bool subtractive = false;
        std::optional<scene::OrientedBox> oriented;   // point entities: the box in its own frame
    };
    struct Node {
        double mn[3], mx[3];
        int left = -1, right = -1;            // children, or -1
        int first = 0, count = 0;             // leaf: items_[order_[first .. first + count)]
    };
    int build(int first, int count);
    void hitItem(const Item& item, const Ray& ray, const PickOptions& opts, std::optional<PickHit>& best) const;

    std::shared_ptr<const scene::SceneSnapshot> snap_;
    scene::BoxOf boxOf_ = scene::classBoxes(nullptr);
    std::vector<Item> items_;
    std::vector<int> order_;
    std::vector<Node> nodes_;
};

} // namespace native::render
