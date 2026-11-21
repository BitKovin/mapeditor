// tools/texturing/browser -- what the texture browser lists (the model behind shell/src/ui/contentBrowser.ts): the folders of the texture list, the sky folder, and the textures
// of a folder that match a search text. The Qt panel (ui/texture_browser_panel) only draws it.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (contentBrowser), 08_MILESTONES.md M11 item 3.
#pragma once

#include <string>
#include <vector>

namespace native::tools::tex {

/// The folder entry that shows every sky texture (skyboxes of env/ and textures the export treats as sky).
inline const char* kSkyFolder = " sky";

/// A texture the engine draws as the sky: a skybox, or a name that says sky (model/sky.ts isSkyTexture).
bool isSkyTexture(const std::string& path);
/// The folder part of a texture path ("a/b/c" -> "a/b"; "" for a top-level texture).
std::string folderOf(const std::string& path);
/// The folder list: "" (all textures), the sky folder when there is a sky texture, then every folder that holds a texture, sorted.
std::vector<std::string> folderEntries(const std::vector<std::string>& all);
/// The textures of `folder` ("" = all, kSkyFolder = the sky textures; else the textures below that folder) whose path contains `query` (case-insensitive).
std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query);
/// The label of a folder entry.
std::string folderLabel(const std::string& folder);

} // namespace native::tools::tex
