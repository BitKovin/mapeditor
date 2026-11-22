// tools/select_ops -- Select all, Invert selection and the arrow-key nudge of the object tool (ports of selectableItems / selectAllItems / invertSelectionItems /
// nudge of shell/src/state/actions.ts and ui/viewport.ts). Pure: the caller picks what to select and commits a nudge.
// Thread: any (pure). Spec: 08_MILESTONES.md M11 item 10 (remaining key bindings).
#pragma once

#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/transform.h"

namespace native::tools {

// Everything that can be selected: each brush of every entity, each point entity (worldspawn and entities with brushes are only their brushes) and each prefab
// instance. Document order.
std::vector<doc::SelRef> selectableItems(const doc::DocumentVersion& doc);

// Everything that is NOT selected now (an entity selected as a whole counts as all of its brushes; a face selection counts as its brush).
std::vector<doc::SelRef> invertedItems(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& selected);

// The Radiant arrow nudge: the way a key moves the selection in the world. `right` / `forward` are the camera's axes; the horizontal one of the two that is
// dominant on the ground (+-X or +-Z; Y is up) is taken. key: left, right, up (forward), down (back), pageup (world up), pagedown (world down).
// Empty (zero) for an unknown key.
V3 nudgeDirection(const std::string& key, const V3& right, const V3& forward);

} // namespace native::tools
