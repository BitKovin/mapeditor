// tools/context_menu -- the entries of the viewport's right-click menus (port of openContextMenu / openMeshMenu of shell/src/ui/viewport.ts).
//
// Pure: a menu is a list of entries that name registry actions (10_UI_AND_INPUT.md: context menus are generated from the registry, so they can never disagree with what is wired);
// the window greys an entry out when its action is not enabled and runs it with the entry's parameters.
// Thread: any (pure).
// Spec: 07_EDITOR_PORT.md (contextMenu.ts), 08_MILESTONES.md T11.10c.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace native::tools {

struct MenuEntry {
    std::string label;
    std::string action;                                  // registry action id; empty for a separator or a submenu
    nlohmann::json params = nlohmann::json::object();    // the parameters it runs with
    bool separator = false;
    std::vector<MenuEntry> sub;                          // a submenu (no action of its own)
};

/// The classes "Make entity" offers: the brush entity classes of the game, sorted; the common four without a definition list.
std::vector<std::string> makeEntityClasses(const std::vector<std::string>& groupClasses);

/// Right-click in object mode on a brush, an entity or nothing: make entity, brush tools, groups, UV, hide, properties and the clipboard. `hiddenCount` shows in "Unhide all (n)".
std::vector<MenuEntry> objectContextMenu(const std::vector<std::string>& groupClasses, size_t hiddenCount);
/// Right-click in mesh mode: the tools that work on the mesh selection as it is.
std::vector<MenuEntry> meshContextMenu();
/// Right-click on another face while one face is selected: copy its texture and UV onto the clicked face, or flow the texture onto it. The two actions take {entityId, brushId, faceId}.
std::vector<MenuEntry> surfaceContextMenu(const std::string& entityId, const std::string& brushId, unsigned faceId);

} // namespace native::tools
