// tools/details -- the Details panel as data (port of the model part of shell/src/ui/panels.ts InspectorPanel): what the selected entity, brush or prefab instance
// shows (rows in the Shell's order: classname, id, targetname, origin, brush, spawnflags, angles, scale, typed properties from the .ent, other keyvalues; for an
// instance its name, transform, contents with the per-entity override editor and the override summary), and what editing each row does (applyDetailsEdit).
// The panel (ui/details_panel) only draws rows and reports the user's edits as (DetailsEdit, EditValue).
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <array>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/entdef.h"
#include "core/prefab.h"
#include "doc/document.h"
#include "doc/prefabs.h"
#include "doc/selection.h"
#include "tools/entity_edit.h"
#include "tools/ref_targets.h"

namespace native::tools {

// What changing a row does.
struct DetailsEdit {
    enum class Kind {
        None,
        Classname,        // text
        Keyvalue,         // text (nullopt text / `remove` removes the key; `emptyRemoves`: an empty text does)
        KeyvalueBool,     // checked -> "1" / "0"
        KeyvalueNumber,   // number, clamped to the .ent range, integers rounded
        KeyvalueVec,      // number into component `axis` of "x y z"
        KeyvalueColor,    // text "#rrggbb" -> "r g b" (0..1, or 0..255 with `color255`)
        Origin, Angle, Scale,   // number into component `axis`
        Ref,              // paths
        ShareName,        // the paths of the row
        BrushOp,          // checked = subtractive
        Carve,            // checked; `axis` = CarveKey
        Shape,            // a parametric shape of the brush: `key` = sides / steps / segments / rings / archSegments / archThickness / smooth / du / dv / h / turn / dissolve
        VertexLit,        // checked
        Spawnflag,        // checked; `bit`
        InstanceName,     // text
        InstanceVec,      // number into component `axis` of origin (0) / angles (1) / scale (2): `field`
        OverrideKeyvalue, // text; `path`, `key` (`remove` reverts)
        OverrideRef,      // paths; `path`, `key`
        OverrideDelete,   // the entity at `path` is deleted in this instance (checked = on)
        OverrideRevert,   // every override of the entity at `path`
        MultiKeyvalue,    // text (as Keyvalue) on every entity of `entityIds`: one undo step
    };
    Kind kind = Kind::None;
    std::string entityId, brushId, instanceId, key, path;
    std::vector<std::string> entityIds;                      // MultiKeyvalue: the entities that get the value
    int axis = 0, bit = 0, field = 0;
    bool remove = false, emptyRemoves = false, color255 = false;
};

struct EditValue {
    std::optional<std::string> text;
    double number = 0;
    bool checked = false;
    std::vector<std::string> paths;
};

enum class RowKind {
    Section,    // a heading
    Note,       // dimmed text
    Text,       // a text field
    Static,     // a read-only text field
    Real, Integer, Vec3, Check, Choice, Color, Asset, Ref,
    KeyValue,   // key + text + remove button
    AddKey,     // new key + value + Add (the key is `value`... see the panel)
    Content,    // one entity of a prefab (click toggles its override editor)
    Summary,    // one override: path, what it overrides, Revert / Restore
};

struct DetailsRow {
    RowKind kind = RowKind::Note;
    std::string label, tip, value;                           // the Section / Note text is `label`; Text, Static, Choice, Asset, Color: `value`
    std::array<std::string, 3> vec;                          // Vec3: the components as text ("" = unset)
    std::vector<std::pair<std::string, std::string>> choices;   // Choice: (label, value)
    std::optional<double> min, max, step;
    bool checked = false;
    bool readOnly = false;
    bool changed = false;                                    // not the default / overridden: drawn marked
    bool mixed = false;                                      // KeyValue of several entities that do not all have the same value (`value` is empty)
    bool resettable = false;                                 // the reset button works
    bool open = false;                                       // Content: its override editor is shown below
    std::vector<std::string> paths;                          // Ref: the targets
    std::string excludePath;                                 // Ref: an entity that cannot be its own target
    std::string warning;                                     // Ref: the targets have different names
    bool canShare = false;                                   // Ref: "Share first name" works
    std::string path;                                        // Content / Summary: the entity path inside the instance
    DetailsEdit edit;                                        // what the row's widget edits
    DetailsEdit reset;                                       // the reset / remove / revert button (None = none)
    std::string resetLabel;                                  // "Revert" / "Restore" of a Summary row
};

struct DetailsForm {
    enum class Mode { Nothing, Entity, Instance, Inside, Multi };   // Multi: two or more document entities selected
    Mode mode = Mode::Nothing;
    std::string message;                                     // Nothing: "Nothing selected."; Inside: why it is not editable
    std::string title;
    std::vector<DetailsRow> rows;
    std::vector<RefTarget> targets;                          // the entity picker's list
    std::vector<std::pair<std::string, std::string>> classes;   // (name, "brush - description") for the classname field
    std::vector<std::string> refKeys;                        // reference keys the add row turns into pickers
    std::string entityId, instanceId;                        // what the form shows
};

struct DetailsContext {
    std::shared_ptr<const doc::DocumentVersion> doc;
    const doc::SelRef* selection = nullptr;                  // the primary selection
    const std::vector<doc::SelRef>* selections = nullptr;    // the whole selection (optional): two or more entities give the Multi form
    const core::EntityDefinitions* defs = nullptr;
    const std::vector<core::ResolvedEntity>* prefab = nullptr;   // the expanded prefab content (Expansion::flat())
    doc::PrefabLibrary* library = nullptr;
    std::set<std::string> pendingRefs;                       // reference keys added with "Add" that have no target yet (shown as an empty picker)
    std::string openPath;                                    // the entity of the selected instance whose override editor is open
    bool readOnly = false;
};

// Is the entity a brush entity (has brushes, or is the world)?
bool isBrushEntityNode(const doc::EntityNode& n);

DetailsForm buildDetails(const DetailsContext& ctx);

// Applies the user's edit of a row to `base`. `defs` is used for the typed keys (range, defaults).
EditResult applyDetailsEdit(const std::shared_ptr<const doc::DocumentVersion>& base, const core::EntityDefinitions* defs, const DetailsEdit& edit, const EditValue& value);

} // namespace native::tools
