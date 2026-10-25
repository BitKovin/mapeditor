// Exporters (REQUIREMENTS §31): the registry and the OBJ exporter. The rBSP exporter is bsp_export.cpp.
#include "core/exporter.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace core {

namespace fs = std::filesystem;
using nlohmann::json;

const std::vector<Exporter>& exporters() {
    static const std::vector<Exporter> list = {
        {"rbsp", "rBSP", ".bsp", [](const ExportRequest& r, const ProgressFn& p, const std::function<bool()>& c) { return exportBsp(r, p, c); }},
        {"obj", "OBJ + JSON", ".obj", [](const ExportRequest& r, const ProgressFn& p, const std::function<bool()>& c) { return exportObj(r, p, c); }},
    };
    return list;
}

const Exporter* findExporter(const std::string& id) {
    for (const Exporter& e : exporters()) if (e.id == id) return &e;
    return nullptr;
}

const Exporter& exporterFor(const std::string& id, std::string* warning) {
    if (const Exporter* e = findExporter(id)) return *e;
    if (warning) *warning = "game_profile.json: unknown exporter '" + id + "', exporting rBSP";
    return exporters().front();
}

std::string withExporterExtension(const std::string& path, const Exporter& e) {
    std::string base = path;
    for (const char* ext : {".map.json", ".json", ".map"})
        if (base.size() > std::strlen(ext) && base.compare(base.size() - std::strlen(ext), std::string::npos, ext) == 0) { base.resize(base.size() - std::strlen(ext)); break; }
    const fs::path p(base);
    if (p.has_extension() && p.extension() != ".") base = (p.parent_path() / p.stem()).generic_string();
    return base + e.extension;
}

// ---- OBJ ------------------------------------------------------------------------------------------------------------------------------------
namespace {

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", std::abs(v) < 1e-9 ? 0.0 : v);
    return b;
}

// The entity lump ("key" "value" pairs in { } blocks) as JSON objects, every value a string.
json entitiesJson(const std::string& text) {
    json out = json::array();
    json cur;
    bool in = false;
    std::vector<std::string> tok;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') { cur = json::object(); in = true; tok.clear(); }
        else if (c == '}') { if (in) out.push_back(cur); in = false; }
        else if (c == '"' && in) {
            const size_t end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            tok.push_back(text.substr(i + 1, end - i - 1));
            if (tok.size() == 2) {
                if (tok[0] != "gridsize") cur[tok[0]] = tok[1];  // the rBSP's light grid in Quake units: the .lightvol has the grid
                tok.clear();
            }
            i = end;
        }
    }
    return out;
}

std::string token(std::string s) {
    for (char& c : s) if (c == ' ' || c == '\t') c = '_';
    return s.empty() ? std::string("none") : s;
}

void writeFile(const fs::path& file, const std::string& bytes) {
    const fs::path tmp = file.string() + ".tmp";
    std::ofstream f(tmp, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) throw std::runtime_error("cannot write " + file.string());
    fs::rename(tmp, file);
}

}  // namespace

ExportResult exportObj(const ExportRequest& req, const ProgressFn& progress, const std::function<bool()>& cancelled) {
    ExportResult r;
    r.format = "obj";
    const auto t0 = std::chrono::steady_clock::now();
    try {
        const std::string out = sandboxedExportPath(req.outPath, ".obj");
        PreparedExport p;
        ExportRequest plain = req;
        plain.rbspLightmapEncoding = false;  // an OBJ has no vertex scale and its readers expect .exr pages
        if (!prepareExport(plain, progress, cancelled, r, p)) return r;
        if (progress) progress("writing files", 0.99);
        BspView v;
        std::string error;
        if (!v.open(p.bsp.bsp, &error)) throw std::runtime_error("the compiled map cannot be read back: " + error);
        const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
        const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
        const auto indexes = v.lump<int32_t>(rbsp::kDrawIndexes);
        const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
        const auto models = v.lump<rbsp::Model>(rbsp::kModels);

        const fs::path objFile = req.assetsRoot / fs::path(out);
        const fs::path stem = objFile.parent_path() / objFile.stem();
        const std::string name = objFile.stem().string();
        // the game's texture names are "<textureRoot>/<editor name>": the OBJ uses the editor's names
        std::string root;
        for (const auto& s : shaders) {
            const std::string n(s.name, strnlen(s.name, sizeof s.name));
            if (const size_t slash = n.find('/'); slash != std::string::npos) { root = n.substr(0, slash + 1); break; }
        }

        std::ostringstream o;
        o << "# " << name << ".obj, exported by the level editor (REQUIREMENTS §31): metres, Y up, faces counter-clockwise\n"
          << "# vt = texture coordinates (V up); vl u v = lightmap coordinates of the base light style in its page, (0,0) = the page's first row\n"
          << "# (not standard OBJ: readers skip it); group name <texture>@lm_NNNN = the page " << name << "/lm_NNNN.exr (no @: no lightmap)\n"
          << "# entities: " << name << ".json (\"model\": \"*N\" = object model_N); light volume: " << name << ".lightvol\n";
        size_t base = 1;  // OBJ indices start at 1; v, vt, vn and vl are written together, so one index serves all
        for (size_t m = 0; m < models.size(); ++m) {
            const rbsp::Model& md = models[m];
            if (md.numSurfaces <= 0) continue;
            o << "o " << (m == 0 ? std::string("world") : "model_" + std::to_string(m)) << "\n";
            // one group per texture and lightmap page, in the order they first appear
            std::vector<std::pair<std::string, std::vector<int>>> groups;
            std::map<std::string, size_t> groupOf;
            for (int s = md.firstSurface; s < md.firstSurface + md.numSurfaces && s < static_cast<int>(surfaces.size()); ++s) {
                const rbsp::Surface& sf = surfaces[static_cast<size_t>(s)];
                if (sf.numIndexes <= 0) continue;
                std::string tex = sf.shader >= 0 && sf.shader < static_cast<int>(shaders.size())
                                      ? std::string(shaders[static_cast<size_t>(sf.shader)].name, strnlen(shaders[static_cast<size_t>(sf.shader)].name, 64)) : std::string();
                if (!root.empty() && tex.rfind(root, 0) == 0) tex = tex.substr(root.size());
                char lm[16] = "";
                if (sf.lightmapNum[0] >= 0) std::snprintf(lm, sizeof lm, "@lm_%04d", sf.lightmapNum[0]);
                const std::string key = token(tex) + lm;
                auto [it, fresh] = groupOf.emplace(key, groups.size());
                if (fresh) groups.push_back({key, {}});
                groups[it->second].second.push_back(s);
            }
            for (const auto& [key, list] : groups) {
                o << "g " << key << "\nusemtl " << key.substr(0, key.find('@')) << "\n";
                for (int s : list) {
                    const rbsp::Surface& sf = surfaces[static_cast<size_t>(s)];
                    for (int k = 0; k < sf.numVerts; ++k) {
                        const rbsp::DrawVert& dv = verts[static_cast<size_t>(sf.firstVert + k)];
                        const Vec3 pos = fromQuake(Vec3{dv.xyz[0], dv.xyz[1], dv.xyz[2]});
                        const Vec3 n{dv.normal[0], dv.normal[2], -dv.normal[1]};  // Quake -> editor axes (fromQuake without the scale)
                        o << "v " << num(pos.x) << ' ' << num(pos.y) << ' ' << num(pos.z) << "\n"
                          << "vt " << num(dv.st[0]) << ' ' << num(1.0 - dv.st[1]) << "\n"
                          << "vn " << num(n.x) << ' ' << num(n.y) << ' ' << num(n.z) << "\n"
                          << "vl " << num(dv.lightmap[0][0]) << ' ' << num(dv.lightmap[0][1]) << "\n";
                    }
                    for (int k = 0; k + 2 < sf.numIndexes; k += 3) {
                        size_t t[3];
                        for (int c = 0; c < 3; ++c) t[c] = base + static_cast<size_t>(indexes[static_cast<size_t>(sf.firstIndex + k + c)]);
                        // Quake's triangles are clockwise seen from the front, OBJ's counter-clockwise
                        o << "f " << t[2] << '/' << t[2] << '/' << t[2] << ' ' << t[1] << '/' << t[1] << '/' << t[1] << ' ' << t[0] << '/' << t[0] << '/' << t[0] << "\n";
                    }
                    base += static_cast<size_t>(sf.numVerts);
                }
            }
        }

        // the light volume: header, then per light style its id and the packed probes (lightvol.h PackedProbe, 9 bytes, x fastest, then y, then z)
        std::string vol;
        if (!p.lightvol.empty()) {
            const LightvolGrid& g = p.lightvol.begin()->second;
            auto put = [&](const void* d, size_t n) { vol.append(static_cast<const char*>(d), n); };
            const uint32_t version = 1;
            const float hdr[4] = {static_cast<float>(g.origin.x), static_cast<float>(g.origin.y), static_cast<float>(g.origin.z), static_cast<float>(g.cell)};
            const int32_t dims[4] = {g.nx, g.ny, g.nz, static_cast<int32_t>(p.lightvol.size())};
            put("LVOL", 4);
            put(&version, 4);
            put(hdr, sizeof hdr);
            put(dims, sizeof dims);
            for (const auto& [style, grid] : p.lightvol) {
                if (grid.nx != g.nx || grid.ny != g.ny || grid.nz != g.nz) throw std::runtime_error("light volume: the style layers have different grids");
                const int32_t id = style;
                put(&id, 4);
                const std::vector<uint8_t> bytes = grid.packed();
                put(bytes.data(), bytes.size());
            }
        }

        fs::create_directories(objFile.parent_path());
        writeLightmapPages(stem, p.bsp.lightmaps);
        writeFile(objFile, o.str());
        writeFile(fs::path(stem.string() + ".json"), entitiesJson(v.entities()).dump(1) + "\n");
        std::error_code ec;
        const fs::path volFile = stem.string() + ".lightvol";
        if (!vol.empty()) writeFile(volFile, vol);
        else fs::remove(volFile, ec);  // no bake: no light volume (an older one would not match)
        r.bspPath = out;
        r.lightmapDir = (fs::path(out).parent_path() / fs::path(out).stem()).generic_string();
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (progress) progress("done", 1);
    return r;
}

}  // namespace core
