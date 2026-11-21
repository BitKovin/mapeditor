// tools/prefabize -- "Turn into prefab" (port of shell/src/state/prefabize.ts): the selection becomes a prefab file (a map document of its own, centred on the selection's pivot) and
// is replaced in the level by ONE instance of that file at the pivot, so nothing moves on screen. Pure document functions; the file name and the writing are the main window's.
// Thread: any.
// Spec: 07_EDITOR_PORT.md B (prefabize.ts), 08_MILESTONES.md M11 item 7.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/edit_ops.h"
#include "tools/transform.h"

namespace native::tools {

struct PrefabExtract {
    doc::Json file;   // the prefab document, ready to be written (the Core's file format)
    V3 pivot{0, 0, 0};
};
/// The selection as a prefab document with its pivot at the origin; nullopt when nothing selected exists. Whole selected entities and the picked brushes of the others go in (with the
/// parametric shape records carried along), selected instances too (top level only). Entity ids are kept, so references between the selected entities still work; references to
/// things left behind are dropped. The pivot and the shifted origins are rounded to millimetres, as in the Shell.
std::optional<PrefabExtract> extractPrefab(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);

/// The level after the swap: the selection removed (a selected worldspawn only loses its brushes), one instance of `file` at `pivot` added and selected. Label "Turn into prefab".
EditResult replaceWithInstance(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& file, const V3& pivot);

/// A prefab file name for what the person typed: assets-relative under `prefabRoot`, forward slashes, ".map.json" added when it has no .json ending; empty with `error` set when the
/// name is unusable (empty, absolute, "..", reserved characters).
std::string prefabFileName(const std::string& prefabRoot, const std::string& typed, std::string& error);

} // namespace native::tools
