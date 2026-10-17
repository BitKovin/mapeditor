// Command line export (REQUIREMENTS §25, §31): the same export as the editor's Export button, without the editor.
// Usage: bsp_export_tool <assetsRoot> <map.map.json> <out> [--no-bake] [--lighting lighting.json] [--max-bake-seconds N]
//   <map> and <out> are relative to the assets root (like everything the Core reads and writes). The format: the one whose extension <out> has
//   (.bsp = rBSP, .obj = OBJ + JSON), else the game profile's.
#include "core/exporter.h"
#include "core/scene.h"
#include <cstdlib>
#include <iostream>

using namespace core;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bsp_export_tool <assetsRoot> <map.map.json> <out.bsp|out.obj> [--no-bake] [--lighting lighting.json] [--max-bake-seconds N]\n";
        return 2;
    }
    BspExportRequest req;
    req.assetsRoot = argv[1];
    req.mapPath = argv[2];
    req.outPath = argv[3];
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-bake") req.bake = false;
        else if (a == "--lighting" && i + 1 < argc) req.lighting = LightingService::loadOptions(argv[++i]);
        else if (a == "--max-bake-seconds" && i + 1 < argc) req.maxBakeSeconds = std::atof(argv[++i]);
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    try {
        Scene scene{req.assetsRoot};
        scene.loadGameProfile();
        scene.openDocument(req.mapPath);
        req.document = scene.documentSyncJson()["doc"];
        const Exporter* byExtension = nullptr;
        for (const Exporter& e : exporters())
            if (req.outPath.size() > e.extension.size() && req.outPath.compare(req.outPath.size() - e.extension.size(), std::string::npos, e.extension) == 0) byExtension = &e;
        std::string warning;
        const Exporter& e = byExtension ? *byExtension : exporterFor(scene.profile().exporter, &warning);
        if (!warning.empty()) std::cerr << "warning: " << warning << "\n";
        req.format = e.id;
        req.outPath = withExporterExtension(req.outPath, e);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::string last;
    const ExportResult r = exporterFor(req.format).run(req, [&](const std::string& stage, double f) {
        if (stage != last) { std::cout << stage << " (" << static_cast<int>(f * 100) << "%)" << std::endl; last = stage; }
    }, {});
    std::cout << r.toJson().dump(1) << std::endl;
    return r.ok ? 0 : 1;
}
