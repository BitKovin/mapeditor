// exporting/import_map -- File > Import Quake Map: a .map file (any Quake family syntax) -> the editor document, through the Core importer (core/map_import.h).
// Thread: any (the UI runs it on the job pool). Spec: 08_MILESTONES.md T9.3.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"

namespace native::exporting {

struct ImportedMap {
    std::shared_ptr<const doc::DocumentVersion> version;   // null on failure; path "" (a new, unsaved map)
    std::string error;
    std::vector<std::string> warnings;                     // what the Core could not convert exactly
    int entities = 0, brushes = 0, patches = 0, lights = 0, skipped = 0;
};

// Texture sizes are looked up in the game folder (its profile) so pixel texture coordinates become repeats.
ImportedMap importQuakeMap(const std::filesystem::path& mapFile, const std::filesystem::path& assetsRoot);

} // namespace native::exporting
