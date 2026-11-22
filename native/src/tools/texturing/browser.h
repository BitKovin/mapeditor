// tools/texturing/browser -- what the texture browser lists (the model behind shell/src/ui/contentBrowser.ts): the folders of the texture list, the sky folder, and the textures
// of a folder that match a search text. The Qt panel (ui/texture_browser_panel) only draws it.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (contentBrowser), 08_MILESTONES.md M11 item 3.
#pragma once

#include <string>
#include <vector>

namespace native::tools::tex {

/// The folder entry that shows every sky texture (skyboxes of env/ and textures the export treats as sky).
inline const char* kSkyFolder = " sky";
/// The folder entries that show the favourite textures and the recently used ones (shown only while the list is not empty).
inline const char* kFavouritesFolder = " favourites";
inline const char* kRecentFolder = " recent";

/// A texture the engine draws as the sky: a skybox, or a name that says sky (model/sky.ts isSkyTexture).
bool isSkyTexture(const std::string& path);
/// The folder part of a texture path ("a/b/c" -> "a/b"; "" for a top-level texture).
std::string folderOf(const std::string& path);
/// The folder list: "" (all textures), the sky folder when there is a sky texture, then every folder that holds a texture, sorted.
std::vector<std::string> folderEntries(const std::vector<std::string>& all);
/// The textures of `folder` ("" = all, kSkyFolder = the sky textures; else the textures below that folder) whose path contains every word of `query` (case-insensitive).
std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query);
/// As folderEntries, with the favourites and the recent folder after "" when they hold something.
std::vector<std::string> folderEntries(const std::vector<std::string>& all, const std::vector<std::string>& favourites, const std::vector<std::string>& recent);
/// As filterTextures; the favourites and recent folders list those of the lists that exist in `all` (in the order of the list: the newest recent first) that match.
std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query, const std::vector<std::string>& favourites, const std::vector<std::string>& recent);
/// `list` with `path` first (moved when already in it), cut to `limit` entries: the recently used list.
std::vector<std::string> pushRecent(std::vector<std::string> list, const std::string& path, size_t limit = 24);
/// `list` with `path` added at the end, or removed when it is in it (the favourites toggle).
std::vector<std::string> toggled(std::vector<std::string> list, const std::string& path);
/// The label of a folder entry.
std::string folderLabel(const std::string& folder);

} // namespace native::tools::tex
