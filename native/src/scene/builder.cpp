// scene/builder -- see builder.h.
#include "scene/builder.h"

#include <core/entity_class.h>

#include <algorithm>
#include <chrono>

#include "base/hash.h"

namespace native::scene {

using Clock = std::chrono::steady_clock;

struct SceneBuilder::BrushInfo {
    doc::Box box;
    bool additive = false;     // a solid brush that gets surfaces
    bool cutter = false;       // a subtractive brush that carves
    uint64_t hash = 0;         // content: BrushNode::contentHash, or a hash of the typed brush for prefab brushes
    std::string classname;     // of the owning entity (the carve scope looks at it)
};

struct SceneBuilder::Build {
    uint64_t generation = 0;
    std::shared_ptr<const doc::DocumentVersion> version;
    std::shared_ptr<const doc::Expansion> prefabs;
    std::shared_ptr<Index> index;                          // every brush of the version (and of the prefab expansion)
    std::vector<ObjectKey> keys;                           // the affected ones: one job each
    std::unordered_map<ObjectKey, ObjectProducts, ObjectKey::Hash> done;
    size_t finished = 0;
};

namespace {

uint64_t mix(uint64_t h, uint64_t v) { return base::hashCombine(h, v); }
uint64_t mixString(uint64_t h, const std::string& s) { return base::hashCombine(h, base::hashString(s)); }
template <class T>
uint64_t mixVector(uint64_t h, const std::vector<T>& v) {
    return v.empty() ? mix(h, 0) : base::hashCombine(h, base::hashBytes(v.data(), v.size() * sizeof(T)));
}

// A hash of everything a brush's surfaces depend on, from its typed value (prefab brushes have no JSON node).
uint64_t hashBrushTyped(const core::Brush& b) {
    uint64_t h = mixString(0, b.id);
    h = mix(h, b.op == core::BrushOp::Add ? 1 : 2);
    h = mix(h, (b.carve.self ? 1u : 0u) | (b.carve.structural ? 2u : 0u) | (b.carve.nonStructural ? 4u : 0u));
    h = mixVector(h, b.vertices);
    for (const core::BrushFace& f : b.faces) {
        h = mix(h, f.id);
        h = mixVector(h, f.loop);
        h = mixString(h, f.texture);
        h = mixVector(h, f.uvs);
        h = mix(h, f.lookHash);
        h = base::hashCombine(h, base::hashBytes(&f.lightmapScale, sizeof f.lightmapScale));
        h = base::hashCombine(h, base::hashBytes(&f.projection, sizeof f.projection));
    }
    h = mixVector(h, b.smoothEdges);
    return h;
}

size_t bytesOf(const std::vector<core::LightSurface>& s) {
    size_t n = sizeof(s) + s.capacity() * sizeof(core::LightSurface);
    for (const auto& x : s) n += x.verts.capacity() * sizeof(core::Vec3) + x.texture.capacity() + x.entityId.capacity() + x.brushId.capacity();
    return n;
}

constexpr double kEps = 1e-3;
bool overlaps(const doc::Box& a, const doc::Box& b) {
    if (!a.valid() || !b.valid()) return false;
    return a.mn.x <= b.mx.x + kEps && b.mn.x <= a.mx.x + kEps && a.mn.y <= b.mx.y + kEps && b.mn.y <= a.mx.y + kEps && a.mn.z <= b.mx.z + kEps &&
           b.mn.z <= a.mx.z + kEps;
}

void mergeInto(doc::ChangeSet& into, const doc::ChangeSet& more) {
    if (into.fromVersion == 0 && into.toVersion == 0) into.fromVersion = more.fromVersion;
    into.toVersion = more.toVersion;
    into.all = into.all || more.all;
    into.brushes.insert(into.brushes.end(), more.brushes.begin(), more.brushes.end());
    into.entities.insert(into.entities.end(), more.entities.begin(), more.entities.end());
}

const std::shared_ptr<const std::vector<core::LightSurface>>& noSurfaces() {
    static const auto empty = std::make_shared<const std::vector<core::LightSurface>>();
    return empty;
}

} // namespace

SceneBuilder::SceneBuilder(base::JobPool& pool, size_t cacheBudgetBytes) : pool_(pool), cache_(cacheBudgetBytes) {}

SceneBuilder::~SceneBuilder() {
    std::unique_lock<std::mutex> lock(mutex_);
    shuttingDown_ = true;
    ++generation_;   // running jobs stop at their next step
    pending_.reset();
    idleCv_.wait(lock, [&] { return outstanding_ == 0; });
}

void SceneBuilder::setListener(Listener l) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(l);
}

std::shared_ptr<const SceneSnapshot> SceneBuilder::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

bool SceneBuilder::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_ == 0 && !pending_;
}

bool SceneBuilder::waitIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return outstanding_ == 0 && !pending_; });
}

BuilderStats SceneBuilder::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BuilderStats s = stats_;
    s.jobs = jobsDone_;
    s.computed = computed_;
    s.cacheHits = hits_;
    s.abandoned = abandoned_;
    return s;
}

std::vector<ObjectKey> SceneBuilder::lastAffected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastAffected_;
}

// ---- submit ------------------------------------------------------------------------------------------------------------------------

void SceneBuilder::submit(std::shared_ptr<const doc::DocumentVersion> version, const doc::ChangeSet& changes,
                          std::shared_ptr<const doc::Expansion> prefabs) {
    std::vector<std::function<void()>> after;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.submits;
        if (shuttingDown_ || !version) return;
        mergeInto(sincePublish_, changes);
        if (outstanding_ > 0) {
            // Jobs of the running build read the core::Document: they are cancelled, and the next build starts when the last one is out.
            if (!pending_) pending_ = Pending{std::move(version), std::move(prefabs), changes};
            else {
                pending_->version = std::move(version);
                pending_->prefabs = std::move(prefabs);
                mergeInto(pending_->changes, changes);
            }
            ++generation_;
            return;
        }
        startBuildLocked(Pending{std::move(version), std::move(prefabs), changes}, after);
    }
    for (auto& f : after) f();
}

namespace {
// The first component of a prefab instance string ("a/b" -> "a"): the id of the instance of the document.
std::string topInstance(const std::string& instance) {
    const size_t slash = instance.find('/');
    return slash == std::string::npos ? instance : instance.substr(0, slash);
}
} // namespace

void SceneBuilder::patchCoreDocLocked(const Pending& p, const doc::ChangeSet& sinceApplied) {
    bool full = !appliedVersion_ || sinceApplied.all;
    if (!full)
        for (const auto& ce : sinceApplied.entities)
            if (ce.instance.empty() && (!ce.before.valid() || !ce.after.valid())) full = true;   // an entity came or went
    std::vector<std::string> refresh;
    if (!full) {
        for (const auto& ce : sinceApplied.entities)
            if (ce.instance.empty() && !ce.entityId.empty()) refresh.push_back(ce.entityId);
        for (const auto& cb : sinceApplied.brushes)
            if (cb.instance.empty()) refresh.push_back(cb.entityId);
        std::sort(refresh.begin(), refresh.end());
        refresh.erase(std::unique(refresh.begin(), refresh.end()), refresh.end());
        for (const std::string& id : refresh) {
            const int node = p.version->findEntity(id);
            auto at = coreIndex_.find(id);
            if (node < 0 || at == coreIndex_.end()) {
                full = true;
                break;
            }
            const doc::EntityNode& n = *p.version->entities[static_cast<size_t>(node)];
            core::Entity e = n.typed;
            e.brushes.clear();
            e.brushes.reserve(n.brushes.size());
            for (const auto& b : n.brushes) e.brushes.push_back(b->brush);
            coreDoc_.entities[at->second] = std::move(e);
        }
    }
    if (full) {
        coreDoc_ = doc::toCoreDocument(*p.version);
        coreIndex_.clear();
        for (size_t i = 0; i < coreDoc_.entities.size(); ++i) coreIndex_.emplace(coreDoc_.entities[i].id, i);
    }
    if (!appliedVersion_ || p.prefabs != appliedExpansion_) coreResolved_ = p.prefabs ? p.prefabs->flat() : std::vector<core::ResolvedEntity>();
    appliedVersion_ = p.version;
    appliedExpansion_ = p.prefabs;
}

void SceneBuilder::startBuildLocked(Pending p, std::vector<std::function<void()>>& afterUnlock) {
    ++stats_.builds;
    auto build = std::make_shared<Build>();
    build->generation = ++generation_;
    build->version = p.version;
    build->prefabs = p.prefabs;

    // the index of everything in the new version
    auto index = std::make_shared<Index>();
    const auto addBrush = [&](ObjectKey key, const core::Brush& brush, uint64_t hash, const std::string& classname) {
        BrushInfo info;
        info.box = doc::brushBox(brush);
        const bool editorOnly = core::EntityClassRegistry::isEditorOnly(classname);
        info.additive = brush.op == core::BrushOp::Add && !editorOnly;
        info.cutter = brush.op == core::BrushOp::Subtract && !editorOnly;
        info.hash = hash;
        info.classname = classname;
        index->emplace(std::move(key), std::move(info));
    };
    for (const auto& en : p.version->entities)
        for (const auto& bn : en->brushes) addBrush({"", en->typed.id, bn->brush.id}, bn->brush, bn->contentHash, en->typed.classname);
    if (p.prefabs)
        for (const auto& inst : p.prefabs->instances)
            for (const core::ResolvedEntity& re : *inst->entities)
                for (const core::Brush& b : re.entity.brushes)
                    addBrush({core::instanceString(re.instancePath), re.entity.id, b.id}, b, hashBrushTyped(b), re.entity.classname);
    build->index = index;

    // what changed since the last complete snapshot (superseded builds included)
    doc::ChangeSet since = sinceBase_;
    mergeInto(since, p.changes);
    sinceBase_ = since;

    // the affected set
    std::unordered_set<ObjectKey, ObjectKey::Hash> affected;
    if (since.all || !base_) {
        for (const auto& [key, info] : *index) affected.insert(key);
    } else {
        struct Dirty {
            ObjectKey key;
            doc::Box before, after;
        };
        std::vector<Dirty> dirty;
        for (const auto& cb : since.brushes) dirty.push_back({{cb.instance, cb.entityId, cb.brushId}, cb.before, cb.after});
        // an entity whose class changed: its brushes carve and are carved by another scope
        for (const auto& ce : since.entities)
            if (ce.instance.empty() && (ce.kinds & doc::Class)) {
                for (const auto* idx : {index.get(), baseIndex_.get()})
                    for (const auto& [key, info] : *idx)
                        if (key.instance.empty() && key.entityId == ce.entityId) dirty.push_back({key, info.box, info.box});
            }
        // a prefab instance whose expansion changed: every brush of it, old and new
        if (p.prefabs != base_->prefabs) {
            std::unordered_set<std::string> changedInstances;
            std::unordered_map<std::string, const doc::InstanceExpansion*> before;
            if (base_->prefabs)
                for (const auto& i : base_->prefabs->instances) before[i->instanceId] = i.get();
            if (p.prefabs)
                for (const auto& i : p.prefabs->instances) {
                    auto it = before.find(i->instanceId);
                    if (it == before.end() || it->second != i.get()) changedInstances.insert(i->instanceId);
                    if (it != before.end()) before.erase(it);
                }
            for (const auto& [id, ptr] : before) changedInstances.insert(id);
            if (!changedInstances.empty())
                for (const auto* idx : {index.get(), baseIndex_.get()})
                    for (const auto& [key, info] : *idx)
                        if (!key.instance.empty() && changedInstances.count(topInstance(key.instance))) dirty.push_back({key, info.box, info.box});
        }
        for (const Dirty& d : dirty) {
            const auto now = index->find(d.key);
            const auto was = baseIndex_->find(d.key);
            if (now != index->end()) affected.insert(d.key);
            const bool cutter = (now != index->end() && now->second.cutter) || (was != baseIndex_->end() && was->second.cutter);
            if (!cutter) continue;
            // a subtractive brush changed (or came, or went): every solid brush it overlaps, at its old and its new place
            for (const auto& [key, info] : *index)
                if (info.additive && (overlaps(info.box, d.before) || overlaps(info.box, d.after))) affected.insert(key);
        }
    }
    build->keys.assign(affected.begin(), affected.end());
    std::sort(build->keys.begin(), build->keys.end());
    lastAffected_ = build->keys;

    patchCoreDocLocked(p, p.changes);

    if (build->keys.empty()) {
        publishLocked(build, true, afterUnlock);
        idleCv_.notify_all();
        return;
    }
    current_ = build;
    outstanding_ = static_cast<int>(build->keys.size());
    for (size_t i = 0; i < build->keys.size(); ++i) pool_.submit([this, build, i] { runJob(build, i); }, 4);
}

// ---- jobs --------------------------------------------------------------------------------------------------------------------------

void SceneBuilder::produce(const Build& build, const ObjectKey& key, ObjectProducts& out) {
    const BrushInfo& info = build.index->at(key);
    out.bounds = info.box;
    out.additive = info.additive;
    if (!info.additive) {
        out.surfaces = noSurfaces();
        out.inputsHash = info.hash;
        return;
    }
    // inputs hash: the brush, its entity's class, and every subtractive brush that can carve it (sorted: the order they appear in does not matter)
    uint64_t h = mixString(mixString(mixString(0, key.instance), key.entityId), key.brushId);
    h = mix(h, info.hash);
    h = mixString(h, info.classname);
    std::vector<uint64_t> cutters;
    for (const core::BrushRef& c : core::carversOf(coreDoc_, coreResolved_, {key.instance, key.entityId, key.brushId})) {
        const auto it = build.index->find(ObjectKey{c.instance, c.entityId, c.brushId});
        const uint64_t ch = it == build.index->end() ? 0 : it->second.hash;
        cutters.push_back(mix(ch, c.instance == key.instance && c.entityId == key.entityId ? 1 : 0));
    }
    std::sort(cutters.begin(), cutters.end());
    for (uint64_t c : cutters) h = mix(h, c);
    out.inputsHash = h;

    if (auto hit = cache_.get(key, h)) {
        out.surfaces = std::move(hit);
        ++hits_;
        return;
    }
    auto surfaces = std::make_shared<std::vector<core::LightSurface>>(core::collectSurfacesFor(
        coreDoc_, coreResolved_, [&](const std::string& i, const std::string& e, const std::string& b) {
            return i == key.instance && e == key.entityId && b == key.brushId;
        }));
    cache_.put(key, h, surfaces, bytesOf(*surfaces));
    out.surfaces = std::move(surfaces);
    ++computed_;
}

void SceneBuilder::runJob(const std::shared_ptr<Build>& build, size_t index) {
    ObjectProducts products;
    const bool abandoned = generation_.load() != build->generation;
    if (!abandoned) {
        try {
            produce(*build, build->keys[index], products);
        } catch (...) {
            products = ObjectProducts{};   // a brush the Core cannot carve: no surfaces (the raw brush is drawn)
            products.surfaces = noSurfaces();
            const BrushInfo& info = build->index->at(build->keys[index]);
            products.bounds = info.box;
            products.additive = info.additive;
        }
    }
    jobFinished(build, index, std::move(products), abandoned);
}

void SceneBuilder::jobFinished(const std::shared_ptr<Build>& build, size_t index, ObjectProducts products, bool abandoned) {
    std::vector<std::function<void()>> after;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (abandoned) {
            ++abandoned_;
        } else {
            ++jobsDone_;
            build->done[build->keys[index]] = std::move(products);
            ++build->finished;
        }
        --outstanding_;
        if (!abandoned && generation_.load() == build->generation) {
            if (build->finished == build->keys.size()) publishLocked(build, true, after);
            else if (Clock::now() - lastPublish_ >= std::chrono::milliseconds(8)) publishLocked(build, false, after);
        }
        if (outstanding_ == 0) {
            current_.reset();
            if (pending_ && !shuttingDown_) {
                Pending next = std::move(*pending_);
                pending_.reset();
                startBuildLocked(std::move(next), after);
            }
            idleCv_.notify_all();
        }
    }
    for (auto& f : after) f();
}

void SceneBuilder::publishLocked(const std::shared_ptr<Build>& build, bool complete, std::vector<std::function<void()>>& afterUnlock) {
    auto snap = std::make_shared<SceneSnapshot>();
    snap->version = build->version->version;
    snap->doc = build->version;
    snap->prefabs = build->prefabs;
    snap->complete = complete;
    snap->objects.reserve(build->index->size());
    for (const auto& [key, info] : *build->index) {
        const auto d = build->done.find(key);
        if (d != build->done.end()) {
            snap->objects.emplace(key, d->second);
        } else if (base_) {
            const auto b = base_->objects.find(key);
            if (b != base_->objects.end()) snap->objects.emplace(key, b->second);
        }
    }
    snap->sincePrevious = std::move(sincePublish_);
    sincePublish_ = doc::ChangeSet();
    ++stats_.publishes;
    if (!complete) ++stats_.partialPublishes;
    lastPublish_ = Clock::now();
    published_ = snap;
    if (complete) {
        base_ = snap;
        baseIndex_ = build->index;
        sinceBase_ = doc::ChangeSet();
        std::unordered_set<ObjectKey, ObjectKey::Hash> keys;
        keys.reserve(build->index->size());
        for (const auto& [key, info] : *build->index) keys.insert(key);
        cache_.retain(keys);
    }
    if (listener_) {
        Listener l = listener_;
        afterUnlock.push_back([l, snap] { l(snap); });
    }
}

} // namespace native::scene
