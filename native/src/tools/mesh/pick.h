// tools/mesh/pick -- picking inside one brush for mesh edit mode (port of shell/src/tools/meshPick.ts): the nearest vertex / edge under the cursor in screen space, the face under a
// ray, rectangle selection. Editing is "x-ray": hidden elements can be picked too, like Blender's X-ray mode. Thread: any. Spec: 08_MILESTONES.md M11 item 1.
#pragma once

#include <functional>
#include <optional>
#include <set>
#include <vector>

#include "tools/mesh/topology.h"

namespace native::tools::mesh {

struct Screen {
    double x = 0, y = 0, depth = 0;
};
using Project = std::function<std::optional<Screen>(const core::Vec3&)>;   // empty when the point is behind the camera
struct Ray {
    core::Vec3 origin, dir;
};

std::optional<uint32_t> pickVertex(const core::Brush& b, const Project& project, double mx, double my, double radius = 10);
// `only` limits the candidates (null = every edge).
std::optional<Edge> pickEdge(const core::Brush& b, const Project& project, double mx, double my, double radius = 8, const std::set<Edge>* only = nullptr);
// The edge of face `faceId` nearest to the cursor on screen, however far (face loop selection picks its direction by it).
std::optional<Edge> nearestFaceEdge(const core::Brush& b, uint32_t faceId, const Project& project, double mx, double my);
// The id of the nearest face along the ray (front-most, as in any solid view).
std::optional<uint32_t> pickFaceId(const core::Brush& b, const Ray& ray);
// Everything of the current element kind whose screen position lies in the rectangle.
MeshSel boxSelect(const core::Brush& b, const Project& project, SelMode mode, double x0, double y0, double x1, double y1);
// Union / toggle of two selections in the given element mode (toggle = shift, like Blender's extend).
MeshSel combineSel(const core::Brush& b, const MeshSel& base, const MeshSel& add, SelMode mode, bool toggle);

} // namespace native::tools::mesh
