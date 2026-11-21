// tools/prefab_preview -- what a prefab looks like in the Place panel (port of shell/src/state/prefabPreview.ts and the picture maths of shell/src/render/icons.ts).
//
// Owns: PrefabPreviews (reads the prefab files of the assets folder on demand, in a library of its own so the open map's library stays small, and expands one instance at the
// origin exactly like a placed one: nested prefabs, overrides, the lot) and iconOf (the expanded content as flat-shaded isometric polygons in a square, painted back to front;
// brushes in a neutral stone tone, point entities as boxes in their class colour). Qt-free: the panel paints the polygons.
// Thread: any (one PrefabPreviews per thread of use).
// Spec: 07_EDITOR_PORT.md B (prefabPreview.ts, icons.ts), 08_MILESTONES.md M11 item 7.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <core/prefab.h>

#include "doc/prefabs.h"
#include "scene/point_box.h"

namespace native::tools {

struct PreviewContent {
    std::shared_ptr<const std::vector<core::ResolvedEntity>> entities;   // null when it cannot be read
    std::string error;                                                   // why, when null (the first problem of the expansion)
};

class PrefabPreviews {
public:
    explicit PrefabPreviews(std::filesystem::path assetsRoot = {});
    /// Another folder, or the files changed on disk: everything is forgotten.
    void setAssetsRoot(const std::filesystem::path& root);
    void clear();
    /// The prefab's content at the origin (cached; the file and what it nests are read on the first call).
    PreviewContent get(const std::string& file);

private:
    std::filesystem::path root_;
    std::shared_ptr<doc::PrefabLibrary> library_;
    std::map<std::string, PreviewContent> built_;
};

/// Bounds of what a set of entities shows: the brushes (not subtractive ones) and the boxes of point entities; nullopt when there is nothing to show.
std::optional<scene::Box> contentBounds(const std::vector<core::ResolvedEntity>& content, const scene::BoxOf& boxOf);

struct IconPoly {
    std::vector<std::array<double, 2>> points;   // pixels of the square, y down
    std::array<double, 3> color{1, 1, 1};        // the shaded fill, 0..1
};
struct IconModel {
    std::vector<IconPoly> polys;   // far to near
    scene::Box bounds;             // of the content, in metres
};
/// The content seen from +x +y +z (yaw 0.75, pitch 0.5 like the Shell), fitted to a `px` square with `margin` px around it. Empty `polys` when there is nothing to show.
/// `colorOf` gives a class colour (null = the default amber); near-black colours are lifted so they show on a dark panel.
using ColorOf = std::function<std::optional<std::array<double, 3>>(const std::string& classname)>;
IconModel iconOf(const std::vector<core::ResolvedEntity>& content, const scene::BoxOf& boxOf, const ColorOf& colorOf, int px, double margin = 4);

} // namespace native::tools
