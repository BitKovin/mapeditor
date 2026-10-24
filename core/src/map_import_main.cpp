// Command line Quake map import (DESIGN §12b): the same conversion as the editor's File > Import, without the editor.
// Usage: map_import_tool <assetsRoot> <in.map> [out.map.json] [--thickness units] [--tolerance units]
//   <assetsRoot> is the game folder: its game_profile.json and textures give the image sizes the texture coordinates need.
//   <in.map> and <out> are ordinary paths; without <out> the document is written next to the map as <name>.map.json.
#include "core/map_import.h"
#include "core/profile.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace core;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: map_import_tool <assetsRoot> <in.map> [out.map.json] [--thickness units] [--tolerance units]\n";
        return 2;
    }
    std::filesystem::path in = argv[2], out;
    std::filesystem::path root = argv[1];
    MapImportOptions opt;
    try {
        opt = importOptionsFor(root, loadProfile(root));
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--thickness" && i + 1 < argc) opt.patchThickness = std::atof(argv[++i]);
        else if (a == "--tolerance" && i + 1 < argc) opt.patchTolerance = std::atof(argv[++i]);
        else if (out.empty() && a.rfind("--", 0) != 0) out = a;
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    if (out.empty()) { out = in; out.replace_extension(".map.json"); }
    try {
        const MapImportResult r = importQuakeMapFile(in, opt);
        std::ofstream f(out, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << r.doc.dump();
        for (const auto& w : r.warnings) std::cerr << "warning: " << w << "\n";
        const MapImportStats& s = r.stats;
        std::cout << out.string() << ": " << s.entities << " entities, " << s.brushes << " brushes, " << s.patches << " patches, " << s.lights << " lights, " << s.faces
                  << " faces; skipped " << s.skipped << ", dropped " << s.droppedEntities << " editor entities, " << r.warnings.size() << " warnings\n";
        return s.skipped ? 3 : 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
