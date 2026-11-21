// tools/texturing/group_tools -- the pure helpers of the texture group UI (port of shell/src/model/groupTools.ts, REQUIREMENTS section 20 T8): the starter templates, the group name
// rule, the slot change in a group's text, the parameter definitions read from the text, and the preview geometry (scale, the texture transform of a cell, the cell under a point).
// Groups are read by Core (core::texplan::parseGroup) and laid out by Core (solveLayout); this file only helps to author and draw them.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (groupTools), 08_MILESTONES.md M11 item 4.
#pragma once

#include <core/texplan.h>

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace native::tools::tex {

struct GroupTemplate {
    std::string id, label;
    std::string text;   // the group file, pretty printed, keys in their order
};
/// "Building facade", "Panel wall" and "Bordered floor": what New group starts from. Each is a valid group file.
const std::vector<GroupTemplate>& groupTemplates();

/// Which tab of the Groups panel a group file belongs to: a "kit" is a simple texture set (wall pieces in rows, 9-slice frame), anything else is an advanced rule group.
enum class GroupTab { Simple, Advanced };
GroupTab groupTabOf(const nlohmann::json& raw);
/// The names that contain `query` (case-insensitive), in order.
std::vector<std::string> filterGroups(const std::vector<std::string>& names, const std::string& query);
/// "<name>_copy", or "<name>_copy2", ... the first that is not in `existing`.
std::string copyName(const std::string& name, const std::vector<std::string>& existing);

/// Letters, digits, _ and - with / for folders, at most 120 characters.
bool validGroupName(const std::string& name);

/// Changes the texture of one slot in the text and returns the new text (pretty printed, keys in order), or nullopt when the text is not a group with that slot. Both slot forms are
/// kept: "wall": "path" and "wall": { "texture": "path" }.
std::optional<std::string> setSlotTexture(const std::string& text, const std::string& slot, const std::string& path);

/// A parameter of a group as the text declares it.
struct ParamDef {
    std::string name, label, type;   // "number" | "bool" | "choice"
    double min = 0, max = 1, step = 0.1, defNumber = 0;
    bool defBool = false;
    std::string defChoice;
    std::vector<std::string> options;
};
std::vector<ParamDef> paramDefsOf(const nlohmann::ordered_json& group);
/// The slots (name, texture path) in file order; both slot forms are read.
std::vector<std::pair<std::string, std::string>> slotsOf(const nlohmann::ordered_json& group);

/// Canvas pixels per metre of the preview.
double previewScale(double canvasW, double canvasH, double w, double h, double margin);
/// The cell under a layout point (metres), or -1. Cells on a shared edge belong to the one that starts there (the later one).
int cellAt(const std::vector<core::texplan::Cell>& cells, double x, double y);
/// The matrix [a b c d e f] (QTransform order) that maps image pixels to canvas pixels so a texture drawn through it sits where the cell's UV map puts it. Canvas x = scale * layout
/// x, canvas y = scale * (faceH - layout y).
std::array<double, 6> previewPatternMatrix(const core::texplan::CellMap& m, double imgW, double imgH, double scale, double faceH);

} // namespace native::tools::tex
