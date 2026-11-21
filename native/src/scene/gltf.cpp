// scene/gltf -- see gltf.h.
#include "scene/gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "core/obj_model.h"

namespace native::scene {

namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

struct Component {
    size_t size;
    double norm;
};

bool componentOf(int type, Component& c) {
    switch (type) {
        case 5120: c = {1, 127}; return true;
        case 5121: c = {1, 255}; return true;
        case 5122: c = {2, 32767}; return true;
        case 5123: c = {2, 65535}; return true;
        case 5125: c = {4, 4294967295.0}; return true;
        case 5126: c = {4, 1}; return true;
    }
    return false;
}

double readComponent(const uint8_t* p, int type) {
    switch (type) {
        case 5120: return static_cast<double>(static_cast<int8_t>(*p));
        case 5121: return static_cast<double>(*p);
        case 5122: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        default: { float v; std::memcpy(&v, p, 4); return v; }
    }
}

size_t widthOfType(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT2") return 4;
    if (t == "MAT3") return 9;
    if (t == "MAT4") return 16;
    return 0;
}

std::vector<uint8_t> base64(const std::string& s) {
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (const char ch : s) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+' || ch == '-') d = 62;
        else if (ch == '/' || ch == '_') d = 63;
        else continue;   // '=' and whitespace
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> dataUri(const Json& j) {
    if (!j.is_string()) return std::nullopt;
    const std::string& u = j.get_ref<const std::string&>();
    if (u.rfind("data:", 0) != 0) return std::nullopt;
    const size_t comma = u.find(',');
    if (comma == std::string::npos) return std::nullopt;
    return base64(u.substr(comma + 1));
}

int intOr(const Json& j, const char* key, int dflt) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : dflt;
}

template <size_t N>
std::array<float, N> floatsOr(const Json& j, const char* key, const std::array<float, N>& dflt) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != N) return dflt;
    std::array<float, N> out{};
    for (size_t i = 0; i < N; ++i) {
        if (!(*it)[i].is_number()) return dflt;
        out[i] = (*it)[i].get<float>();
    }
    return out;
}

struct Data {
    std::vector<float> f;
    std::vector<uint32_t> u;
    size_t width = 0, count = 0;
};

class Reader {
public:
    Reader(const Json& g, const uint8_t* bin, size_t binSize) : g_(g), bin_(bin), binSize_(binSize) {}

    // The bytes of a buffer view.
    std::pair<const uint8_t*, size_t> viewBytes(int viewIndex) {
        const Json& views = array("bufferViews");
        if (viewIndex < 0 || static_cast<size_t>(viewIndex) >= views.size()) fail("bad bufferView");
        const Json& bv = views[static_cast<size_t>(viewIndex)];
        const auto [base, size] = bufferBytes(intOr(bv, "buffer", 0));
        const size_t off = static_cast<size_t>(std::max(0, intOr(bv, "byteOffset", 0)));
        const size_t len = static_cast<size_t>(std::max(0, intOr(bv, "byteLength", 0)));
        if (off > size || len > size - off) fail("a bufferView reaches outside its buffer");
        return {base + off, len};
    }

    // An accessor as plain numbers: floats (normalised integers scaled to 0..1 / -1..1) or raw integers.
    Data read(int index, bool ints = false) {
        const Json& accessors = array("accessors");
        if (index < 0 || static_cast<size_t>(index) >= accessors.size()) fail("bad accessor");
        const Json& a = accessors[static_cast<size_t>(index)];
        const int ctype = intOr(a, "componentType", 0);
        Component comp{};
        const std::string type = a.value("type", std::string());
        const size_t width = widthOfType(type);
        if (!componentOf(ctype, comp) || width == 0) fail("unsupported accessor");
        const size_t count = static_cast<size_t>(std::max(0, intOr(a, "count", 0)));
        if (count > (size_t{1} << 28) / width) fail("an accessor is too large");
        Data d;
        d.width = width;
        d.count = count;
        (ints ? d.u.resize(count * width) : d.f.resize(count * width));
        const bool normalized = a.value("normalized", false);
        if (a.contains("bufferView")) {
            const int viewIndex = intOr(a, "bufferView", -1);
            const auto [bytes, len] = viewBytes(viewIndex);
            const Json& bv = array("bufferViews")[static_cast<size_t>(viewIndex)];
            const int declared = intOr(bv, "byteStride", 0);
            const size_t stride = declared > 0 ? static_cast<size_t>(declared) : comp.size * width;
            const size_t start = static_cast<size_t>(std::max(0, intOr(a, "byteOffset", 0)));
            if (count > 0 && (start + (count - 1) * stride + width * comp.size > len)) fail("an accessor reaches outside its bufferView");
            for (size_t i = 0; i < count; ++i)
                for (size_t k = 0; k < width; ++k) {
                    const double v = readComponent(bytes + start + i * stride + k * comp.size, ctype);
                    if (ints) d.u[i * width + k] = v < 0 ? 0 : static_cast<uint32_t>(v);
                    else d.f[i * width + k] = static_cast<float>(ctype == 5126 || !normalized ? v : std::max(-1.0, v / comp.norm));
                }
        }
        return d;
    }

    const Json& array(const char* key) const {
        static const Json empty = Json::array();
        const auto it = g_.find(key);
        return it != g_.end() && it->is_array() ? *it : empty;
    }

private:
    std::pair<const uint8_t*, size_t> bufferBytes(int bufferIndex) {
        if (bufferIndex == 0 && bin_) return {bin_, binSize_};
        const Json& buffers = array("buffers");
        if (bufferIndex >= 0 && static_cast<size_t>(bufferIndex) < buffers.size()) {
            auto it = decoded_.find(bufferIndex);
            if (it == decoded_.end()) {
                const auto bytes = dataUri(buffers[static_cast<size_t>(bufferIndex)].value("uri", Json()));
                if (bytes) it = decoded_.emplace(bufferIndex, *bytes).first;
            }
            if (it != decoded_.end()) return {it->second.data(), it->second.size()};
        }
        fail("the glTF refers to an external buffer; use a .glb");
    }

    const Json& g_;
    const uint8_t* bin_;
    size_t binSize_;
    std::map<int, std::vector<uint8_t>> decoded_;
};

bool readPrimitive(const Json& p, Reader& rd, int skin, int node, ModelPrimitive& out) {
    const Json attr = p.value("attributes", Json::object());
    if (!attr.contains("POSITION")) return false;
    ModelPrimitive m;
    m.positions = rd.read(intOr(attr, "POSITION", -1)).f;
    const size_t vertices = m.positions.size() / 3;
    const int mode = intOr(p, "mode", 4);
    std::vector<uint32_t> indices;
    if (p.contains("indices")) {
        indices = rd.read(intOr(p, "indices", -1), true).u;
    } else {
        indices.resize(vertices);
        for (size_t i = 0; i < vertices; ++i) indices[i] = static_cast<uint32_t>(i);
    }
    if (mode == 5) {   // strip
        std::vector<uint32_t> o;
        for (size_t i = 0; i + 2 < indices.size(); ++i) {
            if (i % 2) o.insert(o.end(), {indices[i + 1], indices[i], indices[i + 2]});
            else o.insert(o.end(), {indices[i], indices[i + 1], indices[i + 2]});
        }
        indices = std::move(o);
    } else if (mode == 6) {   // fan
        std::vector<uint32_t> o;
        for (size_t i = 1; i + 1 < indices.size(); ++i) o.insert(o.end(), {indices[0], indices[i], indices[i + 1]});
        indices = std::move(o);
    } else if (mode != 4) {
        return false;   // points and lines
    }
    indices.resize(indices.size() - indices.size() % 3);
    for (const uint32_t i : indices)
        if (i >= vertices) fail("an index points outside its vertices");
    m.indices = std::move(indices);
    m.uvs = attr.contains("TEXCOORD_0") ? rd.read(intOr(attr, "TEXCOORD_0", -1)).f : std::vector<float>(vertices * 2, 0.0f);
    m.normals = attr.contains("NORMAL") ? rd.read(intOr(attr, "NORMAL", -1)).f : vertexNormals(m.positions, m.indices);
    if (m.uvs.size() < vertices * 2) m.uvs.resize(vertices * 2, 0.0f);
    if (m.normals.size() < vertices * 3) m.normals = vertexNormals(m.positions, m.indices);
    if (attr.contains("JOINTS_0") && attr.contains("WEIGHTS_0") && skin >= 0) {
        const Data j = rd.read(intOr(attr, "JOINTS_0", -1), true);
        m.weights = rd.read(intOr(attr, "WEIGHTS_0", -1)).f;
        if (j.u.size() >= vertices * 4 && m.weights.size() >= vertices * 4) {
            m.joints.resize(j.u.size());
            for (size_t i = 0; i < j.u.size(); ++i) m.joints[i] = static_cast<uint16_t>(j.u[i]);
        } else {
            m.weights.clear();
        }
    }
    m.node = node;
    m.skin = m.joints.empty() ? -1 : skin;
    const int matIndex = intOr(p, "material", -1);
    const Json& materials = rd.array("materials");
    if (matIndex >= 0 && static_cast<size_t>(matIndex) < materials.size()) {
        const Json& mat = materials[static_cast<size_t>(matIndex)];
        const Json pbr = mat.value("pbrMetallicRoughness", Json::object());
        m.color = floatsOr<4>(pbr, "baseColorFactor", {1, 1, 1, 1});
        const Json tex = pbr.value("baseColorTexture", Json::object());
        const int ti = intOr(tex, "index", -1);
        const Json& textures = rd.array("textures");
        if (ti >= 0 && static_cast<size_t>(ti) < textures.size()) m.image = intOr(textures[static_cast<size_t>(ti)], "source", -1);
        const std::string mode2 = mat.value("alphaMode", std::string("OPAQUE"));
        m.alpha = mode2 == "MASK" ? ModelAlpha::Mask : mode2 == "BLEND" ? ModelAlpha::Blend : ModelAlpha::Opaque;
        m.cutoff = mat.contains("alphaCutoff") && mat["alphaCutoff"].is_number() ? mat["alphaCutoff"].get<float>() : 0.5f;
    }
    out = std::move(m);
    return true;
}

} // namespace

ModelData parseGlb(const uint8_t* data, size_t size) {
    const auto u32 = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, data + o, 4);
        return v;
    };
    if (size < 20 || u32(0) != 0x46546c67) fail("not a binary glTF (.glb) file");
    if (u32(4) != 2) fail("only glTF 2.0 is supported");
    Json json;
    bool haveJson = false;
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
    const size_t end = std::min<size_t>(size, u32(8));
    for (size_t o = 12; o + 8 <= end;) {
        const size_t len = u32(o), type = u32(o + 4), start = o + 8;
        if (len > size - start) fail("a chunk of the .glb reaches past the end of the file");
        if (type == 0x4e4f534a) {
            json = Json::parse(data + start, data + start + len, nullptr, false);
            if (json.is_discarded()) fail("the .glb's JSON chunk is not valid JSON");
            haveJson = true;
        } else if (type == 0x004e4942 && !bin) {
            bin = data + start;
            binSize = len;
        }
        o = start + len + ((4 - (len % 4)) % 4);
    }
    if (!haveJson) fail("the .glb has no JSON chunk");
    return parseGltf(json, bin, binSize);
}

ModelData parseGltf(const Json& g, const uint8_t* bin, size_t binSize) {
    if (!g.is_object()) fail("the glTF is not an object");
    Reader rd(g, bin, binSize);
    ModelData model;

    // images (the bytes only; the viewer decodes them)
    for (const Json& im : rd.array("images")) {
        ModelImage img;
        if (im.contains("bufferView")) {
            const auto [b, n] = rd.viewBytes(intOr(im, "bufferView", -1));
            img.mime = im.value("mimeType", std::string("image/png"));
            img.bytes.assign(b, b + n);
        } else if (const auto bytes = dataUri(im.value("uri", Json()))) {
            const std::string uri = im["uri"].get<std::string>();
            const size_t semi = uri.find(';');
            img.mime = uri.substr(5, (semi == std::string::npos ? uri.find(',') : semi) - 5);
            img.bytes = *bytes;
        }   // an external file: not available from a .glb
        model.images.push_back(std::move(img));
    }

    // nodes
    const Json& jnodes = rd.array("nodes");
    model.nodes.resize(jnodes.size());
    for (size_t i = 0; i < jnodes.size(); ++i) {
        const Json& n = jnodes[i];
        ModelNode& node = model.nodes[i];
        if (n.contains("children") && n["children"].is_array())
            for (const Json& c : n["children"])
                if (c.is_number_integer()) node.children.push_back(c.get<int>());
        node.t = floatsOr<3>(n, "translation", {0, 0, 0});
        node.r = floatsOr<4>(n, "rotation", {0, 0, 0, 1});
        node.s = floatsOr<3>(n, "scale", {1, 1, 1});
        if (n.contains("matrix") && n["matrix"].is_array() && n["matrix"].size() == 16) {
            Mat4 m{};
            for (size_t k = 0; k < 16; ++k) m[k] = n["matrix"][k].is_number() ? n["matrix"][k].get<float>() : 0.0f;
            node.matrix = m;
        }
    }
    for (size_t i = 0; i < model.nodes.size(); ++i)
        for (const int c : model.nodes[i].children)
            if (c >= 0 && static_cast<size_t>(c) < model.nodes.size()) model.nodes[static_cast<size_t>(c)].parent = static_cast<int>(i);
    {
        const Json& scenes = rd.array("scenes");
        const int sceneIndex = intOr(g, "scene", 0);
        if (sceneIndex >= 0 && static_cast<size_t>(sceneIndex) < scenes.size() && scenes[static_cast<size_t>(sceneIndex)].contains("nodes") && scenes[static_cast<size_t>(sceneIndex)]["nodes"].is_array())
            for (const Json& r : scenes[static_cast<size_t>(sceneIndex)]["nodes"])
                if (r.is_number_integer()) model.roots.push_back(r.get<int>());
        if (model.roots.empty())
            for (size_t i = 0; i < model.nodes.size(); ++i)
                if (model.nodes[i].parent < 0) model.roots.push_back(static_cast<int>(i));
    }

    // skins
    for (const Json& s : rd.array("skins")) {
        ModelSkin skin;
        if (s.contains("joints") && s["joints"].is_array())
            for (const Json& j : s["joints"]) skin.joints.push_back(j.is_number_integer() ? j.get<int>() : 0);
        skin.inverseBind.assign(skin.joints.size() * 16, 0.0f);
        const std::vector<float> ibm = s.contains("inverseBindMatrices") ? rd.read(intOr(s, "inverseBindMatrices", -1)).f : std::vector<float>();
        for (size_t j = 0; j < skin.joints.size(); ++j)
            for (size_t k = 0; k < 16; ++k) skin.inverseBind[j * 16 + k] = ibm.size() >= j * 16 + 16 ? ibm[j * 16 + k] : (k % 5 == 0 ? 1.0f : 0.0f);
        model.skins.push_back(std::move(skin));
    }

    // the meshes of the nodes reachable from the scene roots, depth first
    std::vector<int> order;
    {
        std::set<int> seen;
        const std::function<void(int)> visit = [&](int i) {
            if (i < 0 || static_cast<size_t>(i) >= model.nodes.size() || !seen.insert(i).second) return;
            order.push_back(i);
            for (const int c : model.nodes[static_cast<size_t>(i)].children) visit(c);
        };
        for (const int r : model.roots) visit(r);
    }
    const Json& meshes = rd.array("meshes");
    for (const int ni : order) {
        const Json& jn = jnodes[static_cast<size_t>(ni)];
        const int meshIndex = intOr(jn, "mesh", -1);
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= meshes.size()) continue;
        const int skin = intOr(jn, "skin", -1);
        const Json& prims = meshes[static_cast<size_t>(meshIndex)].contains("primitives") ? meshes[static_cast<size_t>(meshIndex)]["primitives"] : rd.array("none");
        if (!prims.is_array()) continue;
        for (const Json& p : prims) {
            ModelPrimitive prim;
            if (readPrimitive(p, rd, skin < static_cast<int>(model.skins.size()) ? skin : -1, ni, prim)) model.primitives.push_back(std::move(prim));
        }
    }

    // animations
    size_t animIndex = 0;
    for (const Json& a : rd.array("animations")) {
        ModelAnimation anim;
        const Json samplers = a.value("samplers", Json::array());
        if (a.contains("channels") && a["channels"].is_array())
            for (const Json& c : a["channels"]) {
                const int si = intOr(c, "sampler", -1);
                if (si < 0 || !samplers.is_array() || static_cast<size_t>(si) >= samplers.size()) continue;
                const Json& sampler = samplers[static_cast<size_t>(si)];
                const Json target = c.value("target", Json::object());
                const std::string path = target.value("path", std::string());
                if (!target.contains("node") || !target["node"].is_number_integer()) continue;
                ModelChannel ch;
                if (path == "translation") ch.path = ChannelPath::Translation;
                else if (path == "rotation") ch.path = ChannelPath::Rotation;
                else if (path == "scale") ch.path = ChannelPath::Scale;
                else continue;
                ch.node = target["node"].get<int>();
                ch.times = rd.read(intOr(sampler, "input", -1)).f;
                ch.values = rd.read(intOr(sampler, "output", -1)).f;
                const std::string interp = sampler.value("interpolation", std::string("LINEAR"));
                ch.interp = interp == "STEP" ? Interpolation::Step : interp == "CUBICSPLINE" ? Interpolation::CubicSpline : Interpolation::Linear;
                if (!ch.times.empty()) anim.duration = std::max(anim.duration, ch.times.back());
                anim.channels.push_back(std::move(ch));
            }
        const std::string name = a.value("name", std::string());
        anim.name = name.empty() ? "animation " + std::to_string(animIndex) : name;
        ++animIndex;
        model.animations.push_back(std::move(anim));
    }

    model.bounds = boundsOfPose(poseModel(model, -1, 0));
    return model;
}

ModelData modelFromObj(const core::ObjModel& obj) {
    ModelData model;
    const size_t groups = std::max({obj.textures.size(), obj.materials.size(), size_t{1}});
    std::vector<ModelPrimitive> prims(groups);
    for (const core::ObjTriangle& t : obj.tris) {
        const size_t g = static_cast<size_t>(std::clamp<int>(t.material, 0, static_cast<int>(groups) - 1));
        ModelPrimitive& p = prims[g];
        for (int k = 0; k < 3; ++k) {
            p.positions.insert(p.positions.end(), {static_cast<float>(t.p[k].x), static_cast<float>(t.p[k].y), static_cast<float>(t.p[k].z)});
            p.normals.insert(p.normals.end(), {static_cast<float>(t.n[k].x), static_cast<float>(t.n[k].y), static_cast<float>(t.n[k].z)});
            p.uvs.insert(p.uvs.end(), {static_cast<float>(t.uv[k].u), static_cast<float>(t.uv[k].v)});
            p.indices.push_back(static_cast<uint32_t>(p.indices.size()));
        }
    }
    for (size_t g = 0; g < groups; ++g) {
        if (prims[g].indices.empty()) continue;
        if (g < obj.textures.size()) prims[g].texture = obj.textures[g];
        model.primitives.push_back(std::move(prims[g]));
    }
    model.bounds = {{obj.mn.x, obj.mn.y, obj.mn.z}, {obj.mx.x, obj.mx.y, obj.mx.z}};
    return model;
}

} // namespace native::scene
