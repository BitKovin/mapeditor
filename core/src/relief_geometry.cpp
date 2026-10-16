#include "core/relief_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace core {

namespace {
constexpr size_t kMaxTriangles = 4000000;

uint64_t mixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

std::vector<unsigned char> decodeBase64(const std::string& s) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const size_t i = chars.find(c);
        if (i == std::string::npos) continue;
        acc = (acc << 6) | static_cast<uint32_t>(i);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF)); }
    }
    return out;
}

Vec3 triNormal(const float* t) {
    const Vec3 a{t[0], t[1], t[2]}, b{t[3], t[4], t[5]}, c{t[6], t[7], t[8]};
    return (b - a).cross(c - a).normalized();
}
}  // namespace

std::string reliefKey(const std::string& instance, const std::string& entityId, const std::string& brushInstance, const std::string& brushId, uint32_t faceId, FaceSide side) {
    return instance + "|" + entityId + "|" + brushInstance + "|" + brushId + "|" + std::to_string(faceId) + (side == FaceSide::Front ? "F" : "B");
}

std::string ReliefMesh::key() const { return reliefKey(instance, entityId, brushInstance, brushId, faceId, side); }

bool ReliefMesh::surfaceAt(const Vec3& p, Hit& out) const {
    if (!bvh || tris.empty()) return false;
    const Vec3 f = footprint(p);
    const double top = highH + 1e-3;
    const Vec3 origin = f + normal * top;
    const TriangleBvh::Hit h = bvh->closestHit(origin, normal * -1.0, top - lowH + 2e-3);
    if (h.t < 0) return false;
    const float* t = &tris[static_cast<size_t>(h.triangle) * 9];
    Vec3 n = triNormal(t);
    if (n.length() < 0.5) return false;
    if (n.dot(normal) < 0) n = n * -1.0;
    out.pos = origin - normal * h.t;
    out.normal = n;
    out.triangle = h.triangle;
    return true;
}

std::shared_ptr<const ReliefMesh> ReliefSet::make(ReliefMesh m) {
    m.bmin = {1e300, 1e300, 1e300};
    m.bmax = {-1e300, -1e300, -1e300};
    m.lowH = 1e300;
    m.highH = -1e300;
    uint64_t h = 1469598103934665603ull;
    for (char c : m.key()) h = mixHash(h, static_cast<uint64_t>(c));
    for (double d : {m.normal.x, m.normal.y, m.normal.z, m.offset}) h = mixHash(h, static_cast<uint64_t>(std::llround(d * 100000.0)));
    for (size_t i = 0; i + 2 < m.tris.size(); i += 3) {
        const Vec3 p{m.tris[i], m.tris[i + 1], m.tris[i + 2]};
        m.bmin = {std::min(m.bmin.x, p.x), std::min(m.bmin.y, p.y), std::min(m.bmin.z, p.z)};
        m.bmax = {std::max(m.bmax.x, p.x), std::max(m.bmax.y, p.y), std::max(m.bmax.z, p.z)};
        const double height = m.normal.dot(p) - m.offset;
        m.lowH = std::min(m.lowH, height);
        m.highH = std::max(m.highH, height);
        h = mixHash(h, static_cast<uint64_t>(std::llround(p.x * 10000.0)));
        h = mixHash(h, static_cast<uint64_t>(std::llround(p.y * 10000.0)));
        h = mixHash(h, static_cast<uint64_t>(std::llround(p.z * 10000.0)));
    }
    m.hash = h;
    OccluderMesh om;
    om.tris = m.tris;
    m.bvh = std::make_shared<TriangleBvh>(om);
    return std::make_shared<const ReliefMesh>(std::move(m));
}

void ReliefSet::add(std::shared_ptr<const ReliefMesh> m) { meshes_[m->key()] = std::move(m); }

bool ReliefSet::assign(const nlohmann::json& message) {
    std::unordered_map<std::string, std::shared_ptr<const ReliefMesh>> next;
    size_t total = 0;
    if (message.contains("faces") && message["faces"].is_array()) {
        for (const auto& f : message["faces"]) {
            try {
                if (!f.is_object() || !f["n"].is_array() || f["n"].size() != 3) continue;
                ReliefMesh m;
                m.instance = f.at("i").get<std::string>();
                m.entityId = f.at("e").get<std::string>();
                m.brushInstance = f.at("bi").get<std::string>();
                m.brushId = f.at("b").get<std::string>();
                m.faceId = f.at("f").get<uint32_t>();
                m.side = f.value("s", 0) == 1 ? FaceSide::Back : FaceSide::Front;
                m.normal = Vec3{f["n"][0].get<double>(), f["n"][1].get<double>(), f["n"][2].get<double>()}.normalized();
                m.offset = f.at("d").get<double>();
                if (m.normal.length() < 0.5 || !std::isfinite(m.offset)) continue;
                const std::vector<unsigned char> bytes = decodeBase64(f.at("v").get<std::string>());
                if (bytes.size() < 36 || bytes.size() % 36 != 0) continue;
                const size_t tris = bytes.size() / 36;
                if (total + tris > kMaxTriangles) break;
                m.tris.resize(tris * 9);
                std::memcpy(m.tris.data(), bytes.data(), bytes.size());
                bool finite = true;
                for (float x : m.tris) if (!std::isfinite(x)) { finite = false; break; }
                if (!finite) continue;
                total += tris;
                auto made = make(std::move(m));
                next[made->key()] = made;
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    bool same = next.size() == meshes_.size();
    if (same)
        for (const auto& kv : next) {
            const auto it = meshes_.find(kv.first);
            if (it == meshes_.end() || it->second->hash != kv.second->hash) { same = false; break; }
        }
    meshes_ = std::move(next);
    return !same;
}

std::shared_ptr<const ReliefMesh> ReliefSet::find(const LightSurface& s) const {
    if (meshes_.empty()) return nullptr;
    const auto it = meshes_.find(reliefKey(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.side));
    if (it == meshes_.end() || s.verts.empty()) return nullptr;
    const ReliefMesh& m = *it->second;
    // the flat face must still be where the Shell computed the relief for (an edit it has not seen yet moves it)
    if (m.normal.dot(s.normal) < 0.999 || std::fabs(m.offset - m.normal.dot(s.verts[0])) > 1e-3) return nullptr;
    return it->second;
}

bool displaceSample(const ReliefMesh& r, const Vec3& pos, const Vec3& nrm, const float* sub, Vec3& outPos, Vec3& outNrm, float* outSub) {
    (void)nrm;
    ReliefMesh::Hit h;
    if (!r.surfaceAt(pos, h)) return false;
    outPos = h.pos;
    outNrm = h.normal;
    if (sub && outSub) {
        std::memcpy(outSub, sub, 12 * sizeof(float));
        Vec3 c{sub[0], sub[1], sub[2]}, u{sub[4], sub[5], sub[6]}, v{sub[8], sub[9], sub[10]};
        ReliefMesh::Hit hc;
        if (r.surfaceAt(c, hc)) {
            // centre of the texel on the relief; the steps follow the tangent plane when the whole texel lies on the same triangle
            bool one = true;
            for (int k = 0; k < 4 && one; ++k) {
                const Vec3 q = c + u * ((k & 1) ? 0.5 : -0.5) + v * ((k & 2) ? 0.5 : -0.5);
                ReliefMesh::Hit hq;
                one = r.surfaceAt(q, hq) && hq.triangle == hc.triangle;
            }
            const double cosT = hc.normal.dot(r.normal);
            Vec3 U = u, V = v;
            if (one && cosT > 0.2) {
                U = u + r.normal * (-(hc.normal.dot(u)) / cosT);
                V = v + r.normal * (-(hc.normal.dot(v)) / cosT);
            }
            outSub[0] = static_cast<float>(hc.pos.x); outSub[1] = static_cast<float>(hc.pos.y); outSub[2] = static_cast<float>(hc.pos.z);
            outSub[4] = static_cast<float>(U.x); outSub[5] = static_cast<float>(U.y); outSub[6] = static_cast<float>(U.z);
            outSub[8] = static_cast<float>(V.x); outSub[9] = static_cast<float>(V.y); outSub[10] = static_cast<float>(V.z);
        }
    }
    return true;
}

}  // namespace core
