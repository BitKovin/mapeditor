// tools/context_menu -- see context_menu.h.
#include "tools/context_menu.h"

#include <algorithm>

namespace native::tools {

namespace {

MenuEntry item(const char* label, const char* action, nlohmann::json params = nlohmann::json::object()) {
    MenuEntry e;
    e.label = label;
    e.action = action;
    e.params = std::move(params);
    return e;
}

MenuEntry separator() {
    MenuEntry e;
    e.separator = true;
    return e;
}

} // namespace

std::vector<std::string> makeEntityClasses(const std::vector<std::string>& groupClasses) {
    std::vector<std::string> out = groupClasses;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) out = {"func_group", "func_detail", "func_door", "func_button"};
    return out;
}

std::vector<MenuEntry> objectContextMenu(const std::vector<std::string>& groupClasses, size_t hiddenCount) {
    MenuEntry make;
    make.label = "Make entity";
    for (const std::string& c : makeEntityClasses(groupClasses)) {
        MenuEntry e;
        e.label = c;
        e.action = "edit.make_entity";
        e.params = nlohmann::json{{"classname", c}};
        make.sub.push_back(std::move(e));
    }
    MenuEntry unhide = item("Unhide all", "edit.unhide_all");
    if (hiddenCount > 0) unhide.label += " (" + std::to_string(hiddenCount) + ")";
    return {
        make,
        item("Move to world", "edit.ungroup"),
        separator(),
        item("Clipper", "brush.clipper"),
        item("Make hollow", "brush.hollow"),
        item("Merge brushes", "brush.merge"),
        item("Select inside", "brush.select_inside"),
        item("Select touching", "brush.select_touching"),
        separator(),
        item("Group (func_group)", "edit.group"),
        item("Turn into prefab...", "edit.prefabize"),
        item("Ungroup", "edit.ungroup"),
        separator(),
        item("Copy UV", "texture.copy_uv"),
        item("Paste UV", "texture.paste_uv"),
        separator(),
        item("Hide", "edit.hide"),
        unhide,
        item("Properties", "view.properties"),
        separator(),
        item("Cut", "edit.cut"),
        item("Copy", "edit.copy"),
        item("Paste", "edit.paste"),
        item("Duplicate", "edit.duplicate"),
        item("Delete", "edit.delete"),
    };
}

std::vector<MenuEntry> meshContextMenu() {
    return {
        item("Mark smooth", "mesh.smooth"),
        item("Clear smooth", "mesh.sharp"),
        separator(),
        item("Bridge edge loops", "mesh.bridge"),
        item("Merge", "mesh.merge"),
        item("Dissolve", "mesh.dissolve"),
        item("Flip normals", "mesh.flip"),
        item("Recalculate normals", "mesh.recalc"),
    };
}

std::vector<MenuEntry> surfaceContextMenu(const std::string& entityId, const std::string& brushId, unsigned faceId) {
    const nlohmann::json face = {{"entityId", entityId}, {"brushId", brushId}, {"faceId", faceId}};
    return {
        item("Copy UV and texture from the selected face", "context.copy_surface", face),
        item("Flow texture from the selected face (follow the shape)", "context.flow_surface", face),
    };
}

} // namespace native::tools
