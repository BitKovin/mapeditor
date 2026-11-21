#include "exporting/import_map.h"

#include <exception>

#include <core/map_import.h>
#include <core/profile.h>

#include "doc/io.h"

namespace native::exporting {

ImportedMap importQuakeMap(const std::filesystem::path& mapFile, const std::filesystem::path& assetsRoot) {
    ImportedMap out;
    try {
        core::MapImportOptions options;
        if (!assetsRoot.empty()) options = core::importOptionsFor(assetsRoot, core::loadProfile(assetsRoot));
        const core::MapImportResult r = core::importQuakeMapFile(mapFile, options);
        out.warnings = r.warnings;
        out.entities = r.stats.entities;
        out.brushes = r.stats.brushes;
        out.patches = r.stats.patches;
        out.lights = r.stats.lights;
        out.skipped = r.stats.skipped;
        const doc::LoadResult loaded = doc::loadDocumentText(r.doc.dump());
        out.version = loaded.version;
        if (!loaded.version) out.error = loaded.error.empty() ? "the converted map is not a valid document" : loaded.error;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace native::exporting
