// doc/prefabs -- see prefabs.h.
#include "doc/prefabs.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace native::doc {

namespace {

std::atomic<uint64_t> g_fileVersion{0};
uint64_t nextFileVersion() { return ++g_fileVersion; }

int64_t timeStamp(const fs::file_time_type& t) { return static_cast<int64_t>(t.time_since_epoch().count()); }

} // namespace

PrefabLibrary::PrefabLibrary(fs::path assetsRoot) : root_(std::move(assetsRoot)) {}

void PrefabLibrary::setAssetsRoot(const fs::path& root) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = root;
    files_.clear();
}

fs::path PrefabLibrary::assetsRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return root_;
}

bool PrefabLibrary::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.empty();
}

std::vector<std::string> PrefabLibrary::cachedPaths() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(files_.size());
    for (const auto& [path, f] : files_) out.push_back(path);
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t PrefabLibrary::versionOf(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(core::normalizeAssetPath(path));
    return it == files_.end() ? 0 : it->second->version;
}

void PrefabLibrary::store(std::shared_ptr<const PrefabFile> f) {
    std::lock_guard<std::mutex> lock(mutex_);
    files_[f->path] = std::move(f);
}

std::shared_ptr<const PrefabFile> PrefabLibrary::load(const std::string& normalized) {
    auto f = std::make_shared<PrefabFile>();
    f->path = normalized;
    f->version = nextFileVersion();
    fs::path root;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = root_;
    }
    if (root.empty()) {
        f->error = "prefab '" + normalized + "': no assets folder";
        return f;
    }
    if (normalized.empty() || normalized.rfind("..", 0) == 0 || fs::path(normalized).is_absolute()) {
        f->error = "prefab '" + normalized + "': the path is outside the assets folder";
        return f;
    }
    const fs::path file = root / fs::path(normalized);
    std::error_code ec;
    const auto mtime = fs::last_write_time(file, ec);
    if (ec) {
        f->error = "prefab '" + normalized + "': file not found";
        return f;
    }
    f->stampTime = timeStamp(mtime);
    f->stampSize = static_cast<uint64_t>(fs::file_size(file, ec));
    ++loads_;
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open the file");
        std::ostringstream ss;
        ss << in.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(ss.str());
        f->doc = std::make_shared<const core::Document>(core::documentFromJson(j));
    } catch (const std::exception& e) {
        f->doc.reset();
        f->error = "prefab '" + normalized + "': " + e.what();
    }
    return f;
}

std::shared_ptr<const PrefabFile> PrefabLibrary::get(const std::string& path) {
    const std::string normalized = core::normalizeAssetPath(path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(normalized);
        if (it != files_.end()) return it->second;
    }
    std::shared_ptr<const PrefabFile> f = load(normalized);   // outside the lock: parsing a big prefab must not block other readers
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = files_.emplace(normalized, f);      // another thread may have loaded it meanwhile: keep the first
    return it->second;
}

std::vector<std::string> PrefabLibrary::refresh() {
    std::vector<std::shared_ptr<const PrefabFile>> current;
    fs::path root;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = root_;
        for (const auto& [path, f] : files_) current.push_back(f);
    }
    std::vector<std::string> changed;
    for (const auto& f : current) {
        if (f->inMemory || root.empty()) continue;
        std::error_code ec;
        const fs::path file = root / fs::path(f->path);
        const auto mtime = fs::last_write_time(file, ec);
        const bool exists = !ec;
        const bool existed = f->stampTime != 0 || f->doc != nullptr;   // it was found when loaded
        bool differs = false;
        if (exists) {
            const uint64_t size = static_cast<uint64_t>(fs::file_size(file, ec));
            differs = !existed || timeStamp(mtime) != f->stampTime || size != f->stampSize;
        } else {
            differs = existed;
        }
        if (!differs) continue;
        std::shared_ptr<const PrefabFile> fresh = load(f->path);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = files_.find(f->path);
            if (it == files_.end() || it->second != f) continue;   // replaced meanwhile (setMemory, setAssetsRoot, another refresh)
            it->second = fresh;
        }
        changed.push_back(f->path);
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

void PrefabLibrary::setMemory(const std::string& path, core::Document doc) {
    auto f = std::make_shared<PrefabFile>();
    f->path = core::normalizeAssetPath(path);
    f->doc = std::make_shared<const core::Document>(std::move(doc));
    f->version = nextFileVersion();
    f->inMemory = true;
    store(std::move(f));
}

void PrefabLibrary::setMemoryError(const std::string& path, std::string error) {
    auto f = std::make_shared<PrefabFile>();
    f->path = core::normalizeAssetPath(path);
    f->error = "prefab '" + f->path + "': " + std::move(error);
    f->version = nextFileVersion();
    f->inMemory = true;
    store(std::move(f));
}

// ---- expansion ---------------------------------------------------------------------------------------------------------------------

std::vector<std::string> Expansion::problems() const {
    std::vector<std::string> out;
    for (const auto& i : instances)
        for (const std::string& p : i->problems) out.push_back(i->instanceId + ": " + p);
    return out;
}

size_t Expansion::entityCount() const {
    size_t n = 0;
    for (const auto& i : instances) n += i->entities->size();
    return n;
}

std::vector<core::ResolvedEntity> Expansion::flat() const {
    std::vector<core::ResolvedEntity> out;
    for (const auto& i : instances) out.insert(out.end(), i->entities->begin(), i->entities->end());
    return out;
}

std::vector<std::string> Expansion::instancesUsing(const std::vector<std::string>& files) const {
    std::vector<std::string> out;
    for (const auto& i : instances)
        for (const std::string& f : files)
            if (i->files.count(f)) {
                out.push_back(i->instanceId);
                break;
            }
    return out;
}

std::shared_ptr<const InstanceExpansion> PrefabResolver::resolveOne(const InstanceNode& node, const std::string& rootPath) {
    ++resolves_;
    auto out = std::make_shared<InstanceExpansion>();
    out->instanceId = node.typed.id;
    out->rootPath = rootPath;
    core::Document one;   // a document of this single instance: its entities come out with the instance path {"root", id, ...}
    one.prefabInstances.push_back(node.typed);
    std::vector<std::shared_ptr<const PrefabFile>> held;   // the loader returns references: they must outlive the resolve
    const core::DocumentLoader loader = [&](const std::string& key) -> const core::Document& {
        std::shared_ptr<const PrefabFile> f = library_.get(key);
        held.push_back(f);
        out->files[f->path] = f->version;
        if (!f->doc) throw std::runtime_error(f->error);
        return *f->doc;
    };
    core::ResolveOptions opts;
    opts.maxDepth = maxDepth_;
    try {
        out->entities = std::make_shared<const std::vector<core::ResolvedEntity>>(core::resolvePrefabs(one, loader, opts, rootPath));
    } catch (const std::exception& e) {
        out->problems.push_back(e.what());
        out->entities = std::make_shared<const std::vector<core::ResolvedEntity>>();
    }
    return out;
}

std::shared_ptr<const Expansion> PrefabResolver::expand(const DocumentVersion& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::make_shared<Expansion>();
    result->documentVersion = version.version;
    std::unordered_map<const InstanceNode*, Entry> next;
    next.reserve(version.instances.size());
    for (const auto& node : version.instances) {
        std::shared_ptr<const InstanceExpansion> exp;
        auto it = cache_.find(node.get());
        if (it != cache_.end() && it->second.result->rootPath == version.path) {
            const InstanceExpansion& cached = *it->second.result;
            const bool current = std::all_of(cached.files.begin(), cached.files.end(),
                                             [&](const auto& f) { return library_.versionOf(f.first) == f.second; });
            if (current) exp = it->second.result;
        }
        if (!exp) exp = resolveOne(*node, version.path);
        next.emplace(node.get(), Entry{node, exp});
        result->instances.push_back(std::move(exp));
    }
    cache_ = std::move(next);   // entries of instances that are not in this version are dropped
    return result;
}

} // namespace native::doc
