// Command line BSP export (REQUIREMENTS §25): the same export as the editor's Export BSP button, without the editor.
// Usage: bsp_export_tool <assetsRoot> <map.map.json> <out.bsp> [--no-bake] [--lighting lighting.json] [--max-bake-seconds N]
//   <map> and <out.bsp> are relative to the assets root (like everything the Core reads and writes).
#include "core/bsp_export.h"
#include "core/scene.h"
#include <cstdlib>
#include <iostream>

using namespace core;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bsp_export_tool <assetsRoot> <map.map.json> <out.bsp> [--no-bake] [--lighting lighting.json] [--max-bake-seconds N]\n";
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
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::string last;
    const BspExportResult r = exportBsp(req, [&](const std::string& stage, double f) {
        if (stage != last) { std::cout << stage << " (" << static_cast<int>(f * 100) << "%)" << std::endl; last = stage; }
    });
    std::cout << r.toJson().dump(1) << std::endl;
    return r.ok ? 0 : 1;
}
