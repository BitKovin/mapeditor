// scene/model + scene/gltf: the glTF reader, the posing (nodes, skins, animations) and the .obj model (T11.9a). Converted from shell/tests/gltf.test.ts in spirit: the glb files are built here.
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/obj_model.h"
#include "scene/gltf.h"
#include "scene/model.h"

using namespace native::scene;
using nlohmann::json;

namespace {

// Builds the binary buffer of a test glTF: every add* appends a bufferView + accessor and returns the accessor index.
struct Builder {
    std::vector<uint8_t> bin;
    json views = json::array(), accessors = json::array();

    int raw(const void* data, size_t bytes, int target = 0) {
        while (bin.size() % 4) bin.push_back(0);
        json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
        if (target) v["target"] = target;
        bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        views.push_back(v);
        return static_cast<int>(views.size()) - 1;
    }
    int floats(const std::vector<float>& v, const char* type, bool minmax = false) {
        const int view = raw(v.data(), v.size() * 4);
        const size_t w = std::string(type) == "SCALAR" ? 1 : std::string(type) == "VEC2" ? 2 : std::string(type) == "VEC3" ? 3 : std::string(type) == "VEC4" ? 4 : 16;
        json a = {{"bufferView", view}, {"componentType", 5126}, {"count", v.size() / w}, {"type", type}};
        (void)minmax;
        accessors.push_back(a);
        return static_cast<int>(accessors.size()) - 1;
    }
    int u16(const std::vector<uint16_t>& v, const char* type = "SCALAR") {
        const int view = raw(v.data(), v.size() * 2);
        const size_t w = std::string(type) == "VEC4" ? 4 : 1;
        accessors.push_back({{"bufferView", view}, {"componentType", 5123}, {"count", v.size() / w}, {"type", type}});
        return static_cast<int>(accessors.size()) - 1;
    }
    int u8x4(const std::vector<uint8_t>& v) {   // JOINTS_0 as unsigned bytes
        const int view = raw(v.data(), v.size());
        accessors.push_back({{"bufferView", view}, {"componentType", 5121}, {"count", v.size() / 4}, {"type", "VEC4"}});
        return static_cast<int>(accessors.size()) - 1;
    }
    json document(json extra) const {
        json g = {{"asset", {{"version", "2.0"}}}, {"buffers", json::array({json{{"byteLength", bin.size()}}})}, {"bufferViews", views}, {"accessors", accessors}};
        for (auto it = extra.begin(); it != extra.end(); ++it) g[it.key()] = it.value();
        return g;
    }
    std::vector<uint8_t> glb(const json& g) const {
        std::string js = g.dump();
        while (js.size() % 4) js.push_back(' ');
        std::vector<uint8_t> b = bin;
        while (b.size() % 4) b.push_back(0);
        const auto put = [](std::vector<uint8_t>& o, uint32_t v) {
            for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        std::vector<uint8_t> out;
        put(out, 0x46546c67);
        put(out, 2);
        put(out, static_cast<uint32_t>(12 + 8 + js.size() + 8 + b.size()));
        put(out, static_cast<uint32_t>(js.size()));
        put(out, 0x4e4f534a);
        out.insert(out.end(), js.begin(), js.end());
        put(out, static_cast<uint32_t>(b.size()));
        put(out, 0x004e4942);
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }
};

const std::vector<float> kTriangle = {0, 0, 0, 1, 0, 0, 0, 1, 0};

ModelData parse(const Builder& b, const json& g) {
    const std::vector<uint8_t> file = b.glb(g);
    return parseGlb(file.data(), file.size());
}

bool near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) < eps; }

} // namespace

TEST_CASE("gltf: a triangle: positions, indices, computed normals, bounds") {
    Builder b;
    const int pos = b.floats(kTriangle, "VEC3");
    const int idx = b.u16({0, 1, 2});
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}, {"indices", idx}}})}}})},
                               {"nodes", json::array({json{{"mesh", 0}}})},
                               {"scenes", json::array({json{{"nodes", json::array({0})}}})}});
    const ModelData m = parse(b, g);
    REQUIRE(m.primitives.size() == 1);
    const ModelPrimitive& p = m.primitives[0];
    CHECK(p.vertexCount() == 3);
    CHECK(p.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK(near(p.normals[2], 1));   // counter clockwise in xy: +z
    CHECK(p.uvs.size() == 6);
    CHECK(p.skin == -1);
    CHECK(p.alpha == ModelAlpha::Opaque);
    CHECK(near(m.bounds.max[0], 1));
    CHECK(near(m.bounds.max[1], 1));
    CHECK(near(m.bounds.min[2], 0));
}

TEST_CASE("gltf: a node transform moves the mesh and counts for the bounds") {
    Builder b;
    const int pos = b.floats(kTriangle, "VEC3");
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})},
                               {"nodes", json::array({json{{"children", json::array({1})}, {"translation", {10, 0, 0}}}, json{{"mesh", 0}, {"scale", {2, 2, 2}}}})},
                               {"scenes", json::array({json{{"nodes", json::array({0})}}})}});
    const ModelData m = parse(b, g);
    REQUIRE(m.primitives.size() == 1);
    CHECK(m.primitives[0].indices == std::vector<uint32_t>{0, 1, 2});   // no indices: 0 1 2
    CHECK(m.nodes[1].parent == 0);
    CHECK(near(m.bounds.min[0], 10));
    CHECK(near(m.bounds.max[0], 12));
    CHECK(near(m.bounds.max[1], 2));
}

TEST_CASE("gltf: a mesh outside the scene is not part of the model") {
    Builder b;
    const int pos = b.floats(kTriangle, "VEC3");
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})},
                               {"nodes", json::array({json{{"name", "empty"}}, json{{"mesh", 0}}})},
                               {"scenes", json::array({json{{"nodes", json::array({0})}}})}});
    CHECK(parse(b, g).primitives.empty());
}

TEST_CASE("gltf: strips and fans become triangle lists") {
    Builder b;
    const int pos = b.floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}, "VEC3");
    const int idx = b.u16({0, 1, 2, 3});
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}, {"indices", idx}, {"mode", 5}},
                                                                                              json{{"attributes", {{"POSITION", pos}}}, {"indices", idx}, {"mode", 6}},
                                                                                              json{{"attributes", {{"POSITION", pos}}}, {"mode", 1}}})}}})},
                               {"nodes", json::array({json{{"mesh", 0}}})}});
    const ModelData m = parse(b, g);
    REQUIRE(m.primitives.size() == 2);   // the line primitive is dropped
    CHECK(m.primitives[0].indices == std::vector<uint32_t>{0, 1, 2, 2, 1, 3});
    CHECK(m.primitives[1].indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
}

TEST_CASE("gltf: material colour, texture image and alpha mode") {
    Builder b;
    const int pos = b.floats(kTriangle, "VEC3");
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}, {"material", 0}}})}}})},
                               {"materials", json::array({json{{"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 0.25, 1, 0.75}}, {"baseColorTexture", {{"index", 0}}}}}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.3}}})},
                               {"textures", json::array({json{{"source", 0}}})},
                               {"images", json::array({json{{"uri", "data:image/png;base64,AQID"}}})},
                               {"nodes", json::array({json{{"mesh", 0}}})}});
    const ModelData m = parse(b, g);
    REQUIRE(m.primitives.size() == 1);
    const ModelPrimitive& p = m.primitives[0];
    CHECK(near(p.color[0], 0.5));
    CHECK(near(p.color[3], 0.75));
    CHECK(p.alpha == ModelAlpha::Mask);
    CHECK(near(p.cutoff, 0.3));
    CHECK(p.image == 0);
    REQUIRE(m.images.size() == 1);
    CHECK(m.images[0].mime == "image/png");
    CHECK(m.images[0].bytes == std::vector<uint8_t>{1, 2, 3});
}

TEST_CASE("gltf: a data URI buffer works for .gltf JSON") {
    const std::vector<float> v = kTriangle;
    std::string b64;
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint8_t* d = reinterpret_cast<const uint8_t*>(v.data());
        const size_t n = v.size() * 4;
        for (size_t i = 0; i < n; i += 3) {
            const uint32_t x = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
            b64 += t[(x >> 18) & 63];
            b64 += t[(x >> 12) & 63];
            b64 += i + 1 < n ? t[(x >> 6) & 63] : '=';
            b64 += i + 2 < n ? t[x & 63] : '=';
        }
    }
    const json g = {{"asset", {{"version", "2.0"}}},
                    {"buffers", json::array({json{{"byteLength", 36}, {"uri", "data:application/octet-stream;base64," + b64}}})},
                    {"bufferViews", json::array({json{{"buffer", 0}, {"byteLength", 36}}})},
                    {"accessors", json::array({json{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}}})},
                    {"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", 0}}}}})}}})},
                    {"nodes", json::array({json{{"mesh", 0}}})}};
    const ModelData m = parseGltf(g, nullptr, 0);
    REQUIRE(m.primitives.size() == 1);
    CHECK(near(m.primitives[0].positions[3], 1));
}

TEST_CASE("gltf: unusable files give readable errors") {
    const uint8_t junk[32] = {};
    CHECK_THROWS_WITH_AS(parseGlb(junk, sizeof junk), "not a binary glTF (.glb) file", std::runtime_error);
    Builder b;
    std::vector<uint8_t> file = b.glb(b.document({}));
    file[4] = 1;
    CHECK_THROWS_WITH_AS(parseGlb(file.data(), file.size()), "only glTF 2.0 is supported", std::runtime_error);
    // an index past the vertices
    Builder c;
    const int pos = c.floats(kTriangle, "VEC3");
    const int idx = c.u16({0, 1, 7});
    const json g = c.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}, {"indices", idx}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}});
    CHECK_THROWS_AS(parse(c, g), std::runtime_error);
    // an external buffer
    const json ext = {{"asset", {{"version", "2.0"}}}, {"buffers", json::array({json{{"byteLength", 4}, {"uri", "x.bin"}}})}, {"bufferViews", json::array({json{{"buffer", 0}, {"byteLength", 4}}})},
                      {"accessors", json::array({json{{"bufferView", 0}, {"componentType", 5126}, {"count", 1}, {"type", "SCALAR"}}})},
                      {"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", 0}}}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}};
    CHECK_THROWS_AS(parseGltf(ext, nullptr, 0), std::runtime_error);
}

namespace {

// A two joint skinned strip: vertices at x = 0 (joint 0) and x = 2 (joint 1), joint 1 is a child of joint 0 raised 1 m; the animation lifts joint 1 and turns joint 0.
ModelData skinnedModel(bool animate) {
    Builder b;
    const int pos = b.floats({0, 0, 0, 2, 0, 0, 0, 1, 0, 2, 1, 0}, "VEC3");
    const int joints = b.u8x4({0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0});
    const int weights = b.floats({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, "VEC4");
    const int idx = b.u16({0, 1, 2, 2, 1, 3});
    std::vector<float> ibm(32, 0.0f);
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 4; ++k) ibm[static_cast<size_t>(j * 16 + k * 5)] = 1;
    const int inverse = b.floats(ibm, "MAT4");
    json extra = {{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}, {"JOINTS_0", joints}, {"WEIGHTS_0", weights}}}, {"indices", idx}}})}}})},
                  {"nodes", json::array({json{{"mesh", 0}, {"skin", 0}}, json{{"children", json::array({2})}, {"name", "root"}}, json{{"children", json::array({3})}, {"name", "j0"}}, json{{"translation", {0, 0, 0}}, {"name", "j1"}}})},
                  {"skins", json::array({json{{"joints", json::array({2, 3})}, {"inverseBindMatrices", inverse}}})},
                  {"scenes", json::array({json{{"nodes", json::array({0, 1})}}})}};
    if (animate) {
        const int times = b.floats({0, 1}, "SCALAR");
        const int values = b.floats({0, 0, 0, 0, 3, 0}, "VEC3");   // joint 1 (j1) goes from 0 to 3 up
        extra["animations"] = json::array({json{{"name", "Lift"}, {"samplers", json::array({json{{"input", times}, {"output", values}}})}, {"channels", json::array({json{{"sampler", 0}, {"target", {{"node", 3}, {"path", "translation"}}}}})}}});
    }
    json g = b.document(extra);
    return parse(b, g);
}

} // namespace

TEST_CASE("model: skinned vertices follow their joints") {
    const ModelData m = skinnedModel(true);
    REQUIRE(m.primitives.size() == 1);
    CHECK(m.primitives[0].skin == 0);
    REQUIRE(m.animations.size() == 1);
    CHECK(m.animations[0].name == "Lift");
    CHECK(near(m.animations[0].duration, 1));
    CHECK(isAnimated(m, 0));
    CHECK_FALSE(isAnimated(m, -1));
    const auto rest = poseModel(m, -1, 0);
    CHECK(near(rest[0].positions[3], 2));   // vertex 1 stays at x = 2
    CHECK(near(rest[0].positions[4], 0));
    const auto half = poseModel(m, 0, 0.5);
    CHECK(near(half[0].positions[1], 0));            // vertex 0 (joint 0) stays
    CHECK(near(half[0].positions[4], 1.5));          // vertex 1 (joint 1) is lifted by half of 3
    CHECK(near(half[0].positions[7], 1));            // vertex 2 (joint 0) stays
    CHECK(near(half[0].positions[3 * 3 + 1], 1 + 1.5));   // vertex 3 (joint 1)
    const auto looped = poseModel(m, 0, 1.5);   // time loops
    CHECK(near(looped[0].positions[3 * 3 + 1], 1 + 1.5));
    CHECK(near(boundsOfPose(half).max[1], 2.5));
}

TEST_CASE("model: animationIndex finds names, ignores case, accepts a number") {
    ModelData m;
    m.animations.resize(2);
    m.animations[0].name = "Idle";
    m.animations[1].name = "Walk";
    CHECK(animationIndex(m, "Walk") == 1);
    CHECK(animationIndex(m, " walk ") == 1);
    CHECK(animationIndex(m, "0") == 0);
    CHECK(animationIndex(m, "5") == -1);
    CHECK(animationIndex(m, "") == -1);
    CHECK(animationIndex(m, "run") == -1);
}

TEST_CASE("model: LINEAR, STEP and CUBICSPLINE sampling") {
    ModelData m;
    ModelNode n;
    m.nodes.push_back(n);
    m.roots = {0};
    ModelAnimation a;
    a.duration = 2;
    ModelChannel c;
    c.node = 0;
    c.path = ChannelPath::Translation;
    c.times = {0, 2};
    c.values = {0, 0, 0, 4, 8, 0};
    a.channels.push_back(c);
    m.animations.push_back(a);
    CHECK(near(nodeWorlds(m, 0, 0.5)[0][12], 1));
    CHECK(near(nodeWorlds(m, 0, 0.5)[0][13], 2));
    m.animations[0].channels[0].interp = Interpolation::Step;
    CHECK(near(nodeWorlds(m, 0, 1.9)[0][12], 0));
    // cubic: in tangent, value, out tangent per key; zero tangents = smoothstep
    m.animations[0].channels[0].interp = Interpolation::CubicSpline;
    m.animations[0].channels[0].values = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0};
    CHECK(near(nodeWorlds(m, 0, 1.0)[0][12], 2));
    CHECK(nodeWorlds(m, 0, 0.5)[0][12] < 1);   // eased
    // rotation: half way between identity and 180 degrees about z is 90 degrees
    ModelChannel r;
    r.node = 0;
    r.path = ChannelPath::Rotation;
    r.times = {0, 2};
    r.values = {0, 0, 0, 1, 0, 0, 1, 0};
    m.animations[0].channels = {r};
    const Mat4 w = nodeWorlds(m, 0, 1.0)[0];
    CHECK(near(w[0], 0, 1e-4));   // cos 90
    CHECK(near(w[1], 1, 1e-4));   // sin 90
}

TEST_CASE("model: modelMatrix = translate * rotate(angles) * scale") {
    const Mat4 m = modelMatrix({1, 2, 3}, {0, 90, 0}, {2, 1, 1});
    CHECK(near(m[12], 1));
    CHECK(near(m[13], 2));
    CHECK(near(m[14], 3));
    // the model's +x (scaled by 2) turns with yaw 90 about y to -z
    CHECK(near(m[0], 0, 1e-5));
    CHECK(near(std::fabs(m[2]), 2, 1e-5));
    const Mat4 id = modelMatrix({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
    CHECK(near(id[0], 1));
    CHECK(near(id[5], 1));
    CHECK(near(id[10], 1));
    CHECK(near(id[15], 1));
}

TEST_CASE("model: an .obj becomes one primitive per material") {
    const std::string text =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\n"
        "usemtl a\nf 1/1 2/2 3/3\nusemtl b\nf 1 3 4\nf 1 4 2\n";
    core::ObjModel obj = core::parseObj(text);
    obj.textures = {"/models/x/a", ""};
    const ModelData m = modelFromObj(obj);
    REQUIRE(m.primitives.size() == 2);
    CHECK(m.primitives[0].texture == "/models/x/a");
    CHECK(m.primitives[0].vertexCount() == 3);
    CHECK(m.primitives[1].vertexCount() == 6);
    CHECK(m.primitives[1].indices.size() == 6);
    CHECK(near(m.bounds.max[2], 1));
}
