#include "core/texgroups.h"
#include <algorithm>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace core {

namespace {
const std::string kSuffix = ".texgroup.json";

bool safeName(const std::string& p) {
    if (p.empty() || p.size() > 200 || p.front() == '/' || p.front() == '\\' || p.back() == '/') return false;
    for (unsigned char c : p) {
        if (c < 0x20 || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    for (const auto& part : fs::path(p)) {
        if (part == ".." || part == ".") return false;
    }
    return p.find("//") == std::string::npos;
}

fs::path fileOf(const fs::path& assetsRoot, const GameProfile& profile, const std::string& name) {
    return assetsRoot / profile.texGroupRoot / (name + kSuffix);
}
}  // namespace

std::vector<std::string> listTexGroups(const fs::path& assetsRoot, const GameProfile& profile, size_t limit) {
    std::set<std::string> found;
    const fs::path root = assetsRoot / profile.texGroupRoot;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {};
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string file = it->path().filename().string();
        if (file.size() <= kSuffix.size() || file.compare(file.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) continue;
        fs::path rel = fs::relative(it->path(), root, ec);
        std::string name = rel.generic_string();
        name.resize(name.size() - kSuffix.size());
        if (safeName(name)) found.insert(name);
        if (found.size() >= limit) break;
    }
    return {found.begin(), found.end()};
}

std::optional<json> readTexGroup(const fs::path& assetsRoot, const GameProfile& profile, const std::string& name) {
    if (!safeName(name)) return std::nullopt;
    const fs::path full = fileOf(assetsRoot, profile, name);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) return std::nullopt;
    const auto size = fs::file_size(full, ec);
    if (ec || size > kTexGroupMaxBytes) return std::nullopt;
    std::ifstream in(full, std::ios::binary);
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

std::string writeTexGroup(const fs::path& assetsRoot, const GameProfile& profile, const std::string& name, const json& group) {
    if (!safeName(name)) return "'" + name + "' is not a usable group name";
    if (!group.is_object()) return "a group must be a JSON object";
    // a rule group has a "root" node, a simple texture set (wall set, 9-slice frame) has a "kit"; the Shell checks what is inside
    if (!group.contains("root") && !group.contains("kit")) return "a group needs a \"root\" node or a \"kit\"";
    const std::string text = group.dump(2) + "\n";
    if (text.size() > kTexGroupMaxBytes) return "the group is larger than 1 MB";
    const fs::path full = fileOf(assetsRoot, profile, name);
    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    if (ec) return "could not create " + full.parent_path().string();
    const fs::path tmp = fs::path(full.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "could not write " + full.filename().string();
        out << text;
        if (!out) return "write failed for " + full.filename().string();
    }
    fs::rename(tmp, full, ec);
    if (ec) { fs::remove(full, ec); fs::rename(tmp, full, ec); }  // Windows: rename onto an existing file
    if (ec) { fs::remove(tmp, ec); return "could not replace " + full.filename().string(); }
    return "";
}

std::string deleteTexGroup(const fs::path& assetsRoot, const GameProfile& profile, const std::string& name) {
    if (!safeName(name)) return "'" + name + "' is not a usable group name";
    std::error_code ec;
    fs::remove(fileOf(assetsRoot, profile, name), ec);
    return ec ? "could not delete the group" : "";
}

}  // namespace core
