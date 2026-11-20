// tools/edit_ops -- the document edits behind Delete, Duplicate, Group / Ungroup and Copy / Cut / Paste (ports of the matching parts of shell/src/state/actions.ts and
// state/clipboard.ts). Each takes the document version and the selection and returns the next version (built with a VersionEditor, so unchanged nodes are shared);
// the caller commits it as one undo step. Selections inside prefab instances and face selections are not edited here (a face selection stands for its brush).
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.3, DESIGN section 8.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"

namespace native::tools {

struct EditResult {
    std::shared_ptr<const doc::DocumentVersion> version;   // null when refused or nothing to do
    std::vector<doc::SelRef> select;                       // what to select afterwards
    std::string error;                                     // why it was refused ("" = nothing to do or fine)
    std::string label;                                     // the undo step's name
    bool ok() const { return version != nullptr; }
};

// Removes the selected brushes, whole entities and prefab instances. A brush whose entity is selected as a whole goes with the entity.
EditResult deleteSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels);
// Copies the selection with fresh ids (brushes into their entity, whole entities as new entities, instances with a new name); the copies are selected.
EditResult duplicateSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels);
// Group: the selected brushes (also those of selected entities) move into a new entity of `classname` (func_group) whose origin is the centre of their bounds.
EditResult makeEntityFromSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& classname);
// Ungroup: the brushes of the selected func_group entities go back into worldspawn. Also "Move to world" for any selected brushes.
EditResult moveSelectionToWorld(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels);

// ---- the clipboard ----
// The clipboard as JSON text (the format of the Shell, so both editors paste each other's copies): {"format": "level-editor/clipboard", "version": 1, "items": [...]}.
inline constexpr const char* kClipboardFormat = "level-editor/clipboard";
inline constexpr int kClipboardVersion = 1;
struct ClipboardPayload {
    doc::Json items = doc::Json::array();
    size_t size() const { return items.size(); }
};
// The selection as clipboard data; nullopt when nothing copyable is selected.
std::optional<ClipboardPayload> buildClipboard(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);
std::string serializeClipboard(const ClipboardPayload& payload);
// nullopt when the text is not (valid) editor clipboard JSON, e.g. ordinary text copied elsewhere.
std::optional<ClipboardPayload> parseClipboard(const std::string& text);
// Adds the clipboard's contents with fresh ids, moved by `offset`.
EditResult pasteInto(const std::shared_ptr<const doc::DocumentVersion>& base, const ClipboardPayload& payload, const core::Vec3& offset = {0, 0, 0});

// A prefab instance name for a new instance of `file`: the first free base, base2, base3... (prefabs/sub/door.map.json -> door).
std::string newInstanceName(const doc::DocumentVersion& doc, const std::string& file);
// The names the instances go by, parallel to DocumentVersion::instances: the explicit name, else the file's base name made unique (door, door2, ...).
std::vector<std::string> instanceDisplayNames(const doc::DocumentVersion& doc);

} // namespace native::tools
