#pragma once
#include "core/bsp_export.h"
#include <functional>
#include <string>
#include <vector>

namespace core {

// Exporters (REQUIREMENTS §31 X1): what File > Export / Play map write. Each one gets the same request (the document, the lighting options, an assets
// relative output path) and writes its files; the game's profile names the one to use (GameProfile::exporter). Adding a format = one entry in exporters().
// They share the preparation: prepareExport (bsp_export.h) runs the final bake and compiles the rBSP in memory, an exporter writes what it needs from that.
using ExportRequest = BspExportRequest;
using ExportResult = BspExportResult;
using ExportFn = std::function<ExportResult(const ExportRequest&, const ProgressFn&, const std::function<bool()>& cancelled)>;

struct Exporter {
    std::string id;         // "rbsp", "obj": the value of game_profile.json "exporter"
    std::string name;       // for menus: "rBSP", "OBJ + JSON"
    std::string extension;  // of the main file: ".bsp", ".obj"
    ExportFn run;
};

const std::vector<Exporter>& exporters();
/// nullptr when the id is unknown.
const Exporter* findExporter(const std::string& id);
/// The profile's exporter, or rBSP when the id is unknown (`warning` says so).
const Exporter& exporterFor(const std::string& id, std::string* warning = nullptr);
/// `path` with its extension replaced by the exporter's ("maps/a.bsp" -> "maps/a.obj"; "maps/a.map.json" -> "maps/a.obj").
std::string withExporterExtension(const std::string& path, const Exporter& e);

/// The OBJ exporter (REQUIREMENTS §31 X3): <name>.obj, <name>.json, <name>.lightvol, <name>/lm_NNNN.exr.
ExportResult exportObj(const ExportRequest& request, const ProgressFn& progress = {}, const std::function<bool()>& cancelled = {});

}  // namespace core
