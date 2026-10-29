#include "core/textures.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace core {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool safePath(const std::string& p) {
    if (p.empty() || p.size() > 260 || p.front() == '/' || p.front() == '\\') return false;
    for (unsigned char c : p) {
        if (c < 0x20 || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    for (const auto& part : fs::path(p)) if (part == "..") return false;
    return true;
}
}  // namespace

bool isRootRelativeTexture(const std::string& name) { return !name.empty() && name.front() == '/'; }

std::optional<fs::path> findTextureFile(const fs::path& assetsRoot, const GameProfile& profile, const std::string& name, std::string* extOut) {
    const bool rooted = isRootRelativeTexture(name);
    const std::string rel = rooted ? name.substr(1) : name;
    if (!safePath(rel)) return std::nullopt;
    const fs::path base = rooted ? assetsRoot : assetsRoot / profile.textureRoot;
    std::vector<std::string> exts = profile.textureExtensions;
    if (rooted)  // a model's image is whatever the .mtl names: the profile's extensions first, then the usual ones
        for (const char* e : {".png", ".jpg", ".jpeg", ".tga"})
            if (std::find_if(exts.begin(), exts.end(), [&](const std::string& x) { return lower(x) == e; }) == exts.end()) exts.push_back(e);
    for (const auto& ext : exts) {
        const fs::path full = base / (rel + ext);
        std::error_code ec;
        if (!fs::is_regular_file(full, ec)) continue;
        if (extOut) *extOut = ext;
        return full;
    }
    return std::nullopt;
}

std::vector<std::string> listTextures(const fs::path& assetsRoot, const GameProfile& profile, size_t limit) {
    std::set<std::string> found;
    for (const auto& s : listSkyboxes(assetsRoot)) found.insert(s);
    const fs::path root = assetsRoot / profile.textureRoot;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {found.begin(), found.end()};
    std::set<std::string> exts;
    for (const auto& e : profile.textureExtensions) exts.insert(lower(e));
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (!exts.count(lower(it->path().extension().string()))) continue;
        fs::path rel = fs::relative(it->path(), root, ec).replace_extension();
        found.insert(rel.generic_string());
        if (found.size() >= limit) break;
    }
    return {found.begin(), found.end()};
}

std::optional<TextureFile> readTexture(const fs::path& assetsRoot, const GameProfile& profile, const std::string& path,
                                        size_t maxBytes) {
    std::string ext;
    const auto full = findTextureFile(assetsRoot, profile, path, &ext);
    if (!full) return std::nullopt;
    std::error_code ec;
    const auto size = fs::file_size(*full, ec);
    if (ec || size > maxBytes) return std::nullopt;
    std::ifstream in(*full, std::ios::binary);
    TextureFile f;
    f.ext = lower(ext);
    f.bytes.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(f.bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return f;
}

bool isCubemapTexture(const std::string& path) {
    const std::string name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    return name.size() >= 4 && name.compare(name.size() - 4, 4, "cube") == 0;
}

namespace {
const char* const kSkyExtensions[] = {".png", ".jpg", ".jpeg"};                  // the engine's order (Quake3Bsp ResolveFaceTextureData)
const char* const kSkyFaces[] = {"_lf", "_rt", "_up", "_dn", "_ft", "_bk"};      // CubemapTexture::DecodeSmart, bgfx faces +X -X +Y -Y +Z -Z

std::optional<TextureFile> readFile(const fs::path& full, size_t maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) return std::nullopt;
    const auto size = fs::file_size(full, ec);
    if (ec || size > maxBytes) return std::nullopt;
    std::ifstream in(full, std::ios::binary);
    TextureFile f;
    f.ext = lower(full.extension().string());
    f.bytes.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(f.bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return f;
}
}  // namespace

std::vector<std::string> listSkyboxes(const fs::path& assetsRoot) {
    std::set<std::string> found;
    std::error_code ec;
    const fs::path env = assetsRoot / "env";
    if (!fs::is_directory(env, ec)) return {};
    for (fs::directory_iterator it(env, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") continue;
        std::string base = it->path().stem().string();
        for (const char* f : kSkyFaces)
            if (base.size() > 3 && base.compare(base.size() - 3, 3, f) == 0) { base.resize(base.size() - 3); break; }
        if (isCubemapTexture(base) && safePath(base)) found.insert("env/" + base);
    }
    return {found.begin(), found.end()};
}

std::optional<SkyboxFiles> readSkybox(const fs::path& assetsRoot, const std::string& path, size_t maxBytes) {
    if (!safePath(path) || !isCubemapTexture(path)) return std::nullopt;
    const std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    const fs::path env = assetsRoot / "env";
    for (const char* ext : kSkyExtensions) {
        SkyboxFiles out;
        for (const char* f : kSkyFaces) {
            auto file = readFile(env / (base + f + ext), maxBytes);
            if (!file) break;
            out.images.push_back(std::move(*file));
        }
        if (out.images.size() == 6) return out;
        if (auto pano = readFile(env / (base + ext), maxBytes)) {
            out.panorama = true;
            out.images = {std::move(*pano)};
            return out;
        }
    }
    return std::nullopt;
}

namespace {
using json = nlohmann::json;
bool finite(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

json sanitizeAxis(const json& a) {
    json out = json::object();
    if (!a.is_object()) return out;
    if (a.contains("mode") && a["mode"].is_string()) {
        const auto m = a["mode"].get<std::string>();
        if (m == "repeat" || m == "mirror" || m == "anchor") out["mode"] = m;
    }
    if (a.contains("anchor") && a["anchor"].is_string()) {
        const auto m = a["anchor"].get<std::string>();
        if (m == "start" || m == "end") out["anchor"] = m;
    }
    if (a.contains("split") && finite(a["split"])) out["split"] = std::clamp(a["split"].get<double>(), 0.02, 0.98);
    if (a.contains("edge") && a["edge"].is_string()) {
        const auto m = a["edge"].get<std::string>();
        if (m == "auto" || m == "wrap" || m == "clamp") out["edge"] = m;
    }
    if (a.contains("fill") && a["fill"].is_string()) {
        const auto m = a["fill"].get<std::string>();
        if (m == "repeat" || m == "mirror") out["fill"] = m;
    }
    if (a.contains("alt")) {
        const json& alt = a["alt"];
        out["alt"] = nullptr;
        if (alt.is_object() && alt.contains("texture") && alt["texture"].is_string() && safePath(alt["texture"].get<std::string>())) {
            json o = {{"texture", alt["texture"]}, {"offset", json::array({0.0, 0.0})}};
            if (alt.contains("offset") && alt["offset"].is_array() && alt["offset"].size() == 2 && finite(alt["offset"][0]) && finite(alt["offset"][1]))
                o["offset"] = json::array({alt["offset"][0].get<double>(), alt["offset"][1].get<double>()});
            out["alt"] = o;
        }
    }
    return out;
}


// A relief mesh (Shell model/relief.ts): vertices [x, y, z] (x, y in 0..1 across one repeat, z depth in metres) and faces as index loops. Anything unusable is
// dropped; no usable face = no relief.
json sanitizeRelief(const json& r) {
    constexpr size_t kMaxVerts = 4096, kMaxFaces = 4096, kMaxLoop = 64;
    constexpr double kMaxDepth = 8.0;
    if (!r.is_object() || !r.contains("vertices") || !r["vertices"].is_array() || !r.contains("faces") || !r["faces"].is_array()) return nullptr;
    json verts = json::array();
    for (const auto& v : r["vertices"]) {
        if (verts.size() >= kMaxVerts) break;
        if (v.is_array() && v.size() == 3 && finite(v[0]) && finite(v[1]) && finite(v[2]))
            verts.push_back(json::array({std::clamp(v[0].get<double>(), 0.0, 1.0), std::clamp(v[1].get<double>(), 0.0, 1.0), std::clamp(v[2].get<double>(), -kMaxDepth, kMaxDepth)}));
        else
            verts.push_back(json::array({0.0, 0.0, 0.0}));  // keeps the indices of the faces valid
    }
    json faces = json::array();
    for (const auto& f : r["faces"]) {
        if (faces.size() >= kMaxFaces) break;
        if (!f.is_array() || f.size() < 3 || f.size() > kMaxLoop) continue;
        bool ok = true;
        std::vector<long long> seen;
        for (const auto& i : f) {
            if (!i.is_number_integer() || i.get<long long>() < 0 || static_cast<size_t>(i.get<long long>()) >= verts.size()) { ok = false; break; }
            for (long long s : seen) if (s == i.get<long long>()) ok = false;
            seen.push_back(i.get<long long>());
            if (!ok) break;
        }
        if (ok) faces.push_back(f);
    }
    if (faces.empty()) return nullptr;
    return {{"vertices", verts}, {"faces", faces}};
}

std::optional<fs::path> sidecarPath(const fs::path& assetsRoot, const GameProfile& profile, const std::string& path) {
    const auto full = findTextureFile(assetsRoot, profile, path);
    if (!full) return std::nullopt;
    return fs::path(full->string() + ".json");
}
}  // namespace

json sanitizeTextureProps(const json& props) {
    json out = json::object();
    if (!props.is_object()) return out;
    if (props.contains("pixelsPerMeter") && finite(props["pixelsPerMeter"]) && props["pixelsPerMeter"].get<double>() > 0)
        out["pixelsPerMeter"] = std::clamp(props["pixelsPerMeter"].get<double>(), 1.0, 4096.0);
    if (props.contains("hotspots") && props["hotspots"].is_array()) {
        json hs = json::array();
        for (const auto& r : props["hotspots"]) {
            if (hs.size() >= 64 || !r.is_object()) continue;
            bool ok = true;
            double v[4] = {0, 0, 0, 0};
            int i = 0;
            for (const char* k : {"x", "y", "w", "h"}) {
                if (!r.contains(k) || !finite(r[k])) { ok = false; break; }
                v[i++] = r[k].get<double>();
            }
            if (!ok) continue;
            const double x = std::clamp(v[0], 0.0, 0.99), y = std::clamp(v[1], 0.0, 0.99);
            hs.push_back({{"x", x}, {"y", y}, {"w", std::clamp(v[2], 0.01, 1.0 - x)}, {"h", std::clamp(v[3], 0.01, 1.0 - y)}});
        }
        out["hotspots"] = hs;
    }
    if (props.contains("relief")) out["relief"] = sanitizeRelief(props["relief"]);  // null = no relief (the stored one is removed)
    if (props.contains("transparency") && props["transparency"].is_object()) {  // REQUIREMENTS §23
        const json& t = props["transparency"];
        json o = json::object();
        const std::string m = t.contains("mode") && t["mode"].is_string() ? t["mode"].get<std::string>() : "auto";
        o["mode"] = (m == "opaque" || m == "cutout" || m == "translucent") ? m : "auto";
        for (const char* k : {"cutoff", "opacity", "tint"})
            if (t.contains(k) && finite(t[k])) o[k] = std::clamp(t[k].get<double>(), 0.0, 1.0);
        out["transparency"] = o;
    }
    if (props.contains("repeat") && props["repeat"].is_object()) {
        json rep = json::object();
        for (const char* k : {"u", "v"})
            if (props["repeat"].contains(k)) rep[k] = sanitizeAxis(props["repeat"][k]);
        out["repeat"] = rep;
    }
    return out;
}

std::optional<json> readTextureProps(const fs::path& assetsRoot, const GameProfile& profile, const std::string& path) {
    const auto side = sidecarPath(assetsRoot, profile, path);
    if (!side) return std::nullopt;
    std::error_code ec;
    if (!fs::is_regular_file(*side, ec) || fs::file_size(*side, ec) > 1u << 20) return std::nullopt;
    std::ifstream in(*side, std::ios::binary);
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

std::string writeTextureProps(const fs::path& assetsRoot, const GameProfile& profile, const std::string& path, const json& props) {
    const auto side = sidecarPath(assetsRoot, profile, path);
    if (!side) return "texture '" + path + "' not found";
    json merged = readTextureProps(assetsRoot, profile, path).value_or(json::object());
    const json clean = sanitizeTextureProps(props);  // a named value: items() of a temporary would dangle
    for (const auto& [k, v] : clean.items()) merged[k] = v;
    // pixelsPerMeter absent in the new props = back to the profile default
    if (props.is_object() && !props.contains("pixelsPerMeter")) merged.erase("pixelsPerMeter");
    const fs::path tmp = fs::path(side->string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "could not write " + side->filename().string();
        out << merged.dump(2) << "\n";
        if (!out) return "write failed for " + side->filename().string();
    }
    std::error_code ec;
    fs::rename(tmp, *side, ec);
    if (ec) { fs::remove(*side, ec); fs::rename(tmp, *side, ec); }  // Windows: rename onto an existing file
    return ec ? "could not replace " + side->filename().string() : "";
}

std::string base64Encode(const std::vector<unsigned char>& b) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4);
    for (size_t i = 0; i < b.size(); i += 3) {
        const unsigned v = (b[i] << 16) | (i + 1 < b.size() ? b[i + 1] << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += i + 1 < b.size() ? T[(v >> 6) & 63] : '=';
        out += i + 2 < b.size() ? T[v & 63] : '=';
    }
    return out;
}

}  // namespace core
