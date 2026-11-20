// tools/profile_form -- see profile_form.h.
#include "tools/profile_form.h"

#include <core/profile.h>

#include <algorithm>
#include <cmath>

namespace native::tools {

namespace fs = std::filesystem;

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&] {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

std::vector<std::string> splitExtensions(const std::string& text) {
    std::vector<std::string> out = splitList(text);
    for (std::string& e : out)
        if (e.front() != '.') e.insert(e.begin(), '.');
    return out;
}

std::string joinList(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += separator;
        out += items[i];
    }
    return out;
}

namespace {
ProfileForm formOf(const core::GameProfile& p) {
    ProfileForm f;
    f.name = p.name;
    f.textureRoot = p.textureRoot;
    f.mapRoot = p.mapRoot;
    f.prefabRoot = p.prefabRoot;
    f.texGroupRoot = p.texGroupRoot;
    f.entityDefinitions = p.entityDefinitions;
    f.textureExtensions = joinList(p.textureExtensions, ", ");
    f.modelExtensions = joinList(p.modelExtensions, ", ");
    f.pixelsPerMeter = p.pixelsPerMeter;
    f.exporter = p.exporter == "obj" ? "obj" : "rbsp";
    f.blockLight = joinList(p.blockLight, "\n");
    f.receiveBakedLight = joinList(p.receiveBakedLight, "\n");
    return f;
}
} // namespace

LoadedForm loadProfileForm(const fs::path& assetsRoot) {
    LoadedForm r;
    std::error_code ec;
    r.fileExists = fs::exists(assetsRoot / "game_profile.json", ec);
    try {
        r.form = formOf(core::loadProfile(assetsRoot));
    } catch (const std::exception& e) {
        r.error = e.what();
        r.form = formOf(core::GameProfile{});
    }
    return r;
}

std::string checkRelativePath(const std::string& path, const char* what) {
    if (path.empty()) return std::string(what) + ": empty";
    const fs::path p(path);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory() || path.front() == '/' || path.front() == '\\')
        return std::string(what) + ": must be inside the game folder (a relative path)";
    for (const auto& part : p)
        if (part == "..") return std::string(what) + ": must stay inside the game folder (no \"..\")";
    return {};
}

std::vector<std::string> validateProfileForm(const ProfileForm& f) {
    std::vector<std::string> out;
    const auto add = [&](std::string s) { if (!s.empty()) out.push_back(std::move(s)); };
    add(checkRelativePath(f.textureRoot, "Textures"));
    add(checkRelativePath(f.mapRoot, "Maps"));
    add(checkRelativePath(f.prefabRoot, "Prefabs"));
    add(checkRelativePath(f.texGroupRoot, "Texture groups"));
    add(checkRelativePath(f.entityDefinitions, "Entity definitions"));
    if (f.name.empty()) out.push_back("Name: empty");
    if (!(std::isfinite(f.pixelsPerMeter) && f.pixelsPerMeter > 0)) out.push_back("Texture scale: must be a number above 0");
    if (splitExtensions(f.textureExtensions).empty()) out.push_back("Texture extensions: at least one is needed");
    if (f.exporter != "rbsp" && f.exporter != "obj") out.push_back("Export format: rbsp or obj");
    return out;
}

nlohmann::json profileChanges(const ProfileForm& a, const ProfileForm& b) {
    nlohmann::json c = nlohmann::json::object();
    if (a.name != b.name) c["name"] = b.name;
    if (a.textureRoot != b.textureRoot) c["textureRoot"] = b.textureRoot;
    if (a.mapRoot != b.mapRoot) c["mapRoot"] = b.mapRoot;
    if (a.prefabRoot != b.prefabRoot) c["prefabRoot"] = b.prefabRoot;
    if (a.texGroupRoot != b.texGroupRoot) c["texGroupRoot"] = b.texGroupRoot;
    if (a.entityDefinitions != b.entityDefinitions) c["entityDefinitions"] = b.entityDefinitions;
    if (splitExtensions(a.textureExtensions) != splitExtensions(b.textureExtensions)) c["textureExtensions"] = splitExtensions(b.textureExtensions);
    if (splitExtensions(a.modelExtensions) != splitExtensions(b.modelExtensions)) c["modelExtensions"] = splitExtensions(b.modelExtensions);
    if (a.pixelsPerMeter != b.pixelsPerMeter) c["pixelsPerMeter"] = b.pixelsPerMeter;
    if (a.exporter != b.exporter) c["exporter"] = b.exporter;
    nlohmann::json lighting = nlohmann::json::object();
    if (splitList(a.blockLight) != splitList(b.blockLight)) lighting["blockLight"] = splitList(b.blockLight);
    if (splitList(a.receiveBakedLight) != splitList(b.receiveBakedLight)) lighting["receiveBakedLight"] = splitList(b.receiveBakedLight);
    if (!lighting.empty()) c["lighting"] = lighting;
    return c;
}

std::string saveProfileForm(const fs::path& assetsRoot, const ProfileForm& original, const ProfileForm& edited) {
    const std::vector<std::string> problems = validateProfileForm(edited);
    if (!problems.empty()) return problems.front();
    const nlohmann::json changes = profileChanges(original, edited);
    if (changes.empty()) return {};
    return core::writeProfileFile(assetsRoot, changes);
}

} // namespace native::tools
