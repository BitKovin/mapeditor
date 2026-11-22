// tools/texturing/browser -- see browser.h.
#include "tools/texturing/browser.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace native::tools::tex {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool endsWith(const std::string& s, const std::string& suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }
} // namespace

bool isSkyTexture(const std::string& path) {
    if (path.empty()) return false;
    const std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    const std::string l = lower(path);
    return endsWith(last, "cube") || endsWith(l, "cube") || l.find("sky") != std::string::npos;
}

std::string folderOf(const std::string& path) {
    const size_t i = path.rfind('/');
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

std::vector<std::string> folderEntries(const std::vector<std::string>& all) {
    std::set<std::string> folders;
    bool sky = false;
    for (const std::string& p : all) {
        const std::string f = folderOf(p);
        if (!f.empty()) folders.insert(f);
        if (!sky && isSkyTexture(p)) sky = true;
    }
    std::vector<std::string> out = {""};
    if (sky) out.push_back(kSkyFolder);
    out.insert(out.end(), folders.begin(), folders.end());
    return out;
}

namespace {
std::vector<std::string> words(const std::string& query) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : lower(query)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}
bool matches(const std::string& path, const std::vector<std::string>& ws) {
    const std::string l = lower(path);
    return std::all_of(ws.begin(), ws.end(), [&](const std::string& w) { return l.find(w) != std::string::npos; });
}
} // namespace

std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query) {
    const std::vector<std::string> ws = words(query);
    const std::string prefix = folder + "/";
    std::vector<std::string> out;
    for (const std::string& p : all) {
        const bool inFolder = folder.empty() || (folder == kSkyFolder ? isSkyTexture(p) : p.compare(0, prefix.size(), prefix) == 0);
        if (inFolder && matches(p, ws)) out.push_back(p);
    }
    return out;
}

std::vector<std::string> folderEntries(const std::vector<std::string>& all, const std::vector<std::string>& favourites, const std::vector<std::string>& recent) {
    std::vector<std::string> out = folderEntries(all);
    const std::set<std::string> have(all.begin(), all.end());
    const auto any = [&](const std::vector<std::string>& l) { return std::any_of(l.begin(), l.end(), [&](const std::string& p) { return have.count(p) != 0; }); };
    std::vector<std::string> special;
    if (any(favourites)) special.push_back(kFavouritesFolder);
    if (any(recent)) special.push_back(kRecentFolder);
    out.insert(out.begin() + 1, special.begin(), special.end());
    return out;
}

std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query, const std::vector<std::string>& favourites, const std::vector<std::string>& recent) {
    if (folder != kFavouritesFolder && folder != kRecentFolder) return filterTextures(all, folder, query);
    const std::set<std::string> have(all.begin(), all.end());
    const std::vector<std::string> ws = words(query);
    std::vector<std::string> out;
    for (const std::string& p : folder == kFavouritesFolder ? favourites : recent)
        if (have.count(p) && matches(p, ws) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

std::vector<std::string> pushRecent(std::vector<std::string> list, const std::string& path, size_t limit) {
    if (path.empty()) return list;
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > limit) list.resize(limit);
    return list;
}

std::vector<std::string> toggled(std::vector<std::string> list, const std::string& path) {
    if (path.empty()) return list;
    const auto it = std::find(list.begin(), list.end(), path);
    if (it != list.end()) list.erase(it);
    else list.push_back(path);
    return list;
}

std::string folderLabel(const std::string& folder) {
    return folder.empty() ? "All textures" : folder == kSkyFolder ? "Skyboxes" : folder == kFavouritesFolder ? "\xE2\x98\x85 Favourites" : folder == kRecentFolder ? "Recent" : folder;
}

} // namespace native::tools::tex
