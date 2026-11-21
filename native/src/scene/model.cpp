// scene/model -- see model.h.
#include "scene/model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace native::scene {

namespace {

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 multiply(const Mat4& a, const Mat4& b) {   // a * b, column major
    Mat4 o{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[static_cast<size_t>(k * 4 + r)] * b[static_cast<size_t>(c * 4 + k)];
            o[static_cast<size_t>(c * 4 + r)] = s;
        }
    return o;
}

// gl-matrix mat4.fromRotationTranslationScale
Mat4 fromTrs(const std::array<float, 3>& t, const std::array<float, 4>& q, const std::array<float, 3>& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2, wx = w * x2, wy = w * y2, wz = w * z2;
    return {(1 - (yy + zz)) * s[0], (xy + wz) * s[0], (xz - wy) * s[0], 0,
            (xy - wz) * s[1], (1 - (xx + zz)) * s[1], (yz + wx) * s[1], 0,
            (xz + wy) * s[2], (yz - wx) * s[2], (1 - (xx + yy)) * s[2], 0,
            t[0], t[1], t[2], 1};
}

// gl-matrix quat.slerp (shortest path, no final normalisation)
std::array<float, 4> slerp(const std::array<float, 4>& a, const std::array<float, 4>& b0, float t) {
    std::array<float, 4> b = b0;
    float cosom = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (cosom < 0) {
        cosom = -cosom;
        for (float& v : b) v = -v;
    }
    float s0 = 1 - t, s1 = t;
    if (1 - cosom > 1e-6f) {
        const float omega = std::acos(cosom), sinom = std::sin(omega);
        s0 = std::sin((1 - t) * omega) / sinom;
        s1 = std::sin(t * omega) / sinom;
    }
    return {s0 * a[0] + s1 * b[0], s0 * a[1] + s1 * b[1], s0 * a[2] + s1 * b[2], s0 * a[3] + s1 * b[3]};
}

size_t widthOf(ChannelPath p) { return p == ChannelPath::Rotation ? 4 : 3; }

// The sampled value of a channel at `t`, written into `out` (3 or 4 floats).
void sampleChannel(const ModelChannel& c, float t, float* out) {
    const size_t width = widthOf(c.path);
    const size_t n = c.times.size();
    if (n == 0) return;
    const bool cubic = c.interp == Interpolation::CubicSpline;
    const auto valueAt = [&](size_t k) -> const float* {
        const size_t o = cubic ? k * width * 3 + width : k * width;
        return o + width <= c.values.size() ? &c.values[o] : nullptr;
    };
    const auto copy = [&](const float* v) {
        if (v) std::copy(v, v + width, out);
    };
    if (t <= c.times[0] || n == 1) return copy(valueAt(0));
    if (t >= c.times[n - 1]) return copy(valueAt(n - 1));
    size_t k = 0;
    while (k + 2 < n && c.times[k + 1] <= t) ++k;
    const float t0 = c.times[k], t1 = c.times[k + 1], dt = t1 - t0, u = dt > 0 ? (t - t0) / dt : 0;
    if (c.interp == Interpolation::Step) return copy(valueAt(k));
    if (cubic) {
        const auto base = [&](size_t i) { return i * width * 3; };
        if (base(k + 1) + width * 3 > c.values.size()) return;
        const float *inTan1 = &c.values[base(k + 1)], *v0 = &c.values[base(k) + width], *outTan0 = &c.values[base(k) + 2 * width], *v1 = &c.values[base(k + 1) + width];
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        float r[4] = {0, 0, 0, 0};
        for (size_t j = 0; j < width; ++j) r[j] = h00 * v0[j] + h10 * dt * outTan0[j] + h01 * v1[j] + h11 * dt * inTan1[j];
        if (width == 4) {
            const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
            const float inv = l > 0 ? 1 / l : 1;
            for (float& v : r) v *= inv;
        }
        std::copy(r, r + width, out);
        return;
    }
    const float *a = valueAt(k), *b = valueAt(k + 1);
    if (!a || !b) return;
    if (width == 4) {
        const auto q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
        std::copy(q.begin(), q.end(), out);
    } else {
        for (int j = 0; j < 3; ++j) out[j] = a[j] + (b[j] - a[j]) * u;
    }
}

} // namespace

int animationIndex(const ModelData& model, const std::string& name) {
    const auto trim = [](const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    const std::string n = trim(name);
    if (n.empty() || model.animations.empty()) return -1;
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    for (size_t i = 0; i < model.animations.size(); ++i)
        if (model.animations[i].name == n) return static_cast<int>(i);
    const std::string ln = lower(n);
    for (size_t i = 0; i < model.animations.size(); ++i)
        if (lower(model.animations[i].name) == ln) return static_cast<int>(i);
    if (std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && n.size() < 9) {
        const size_t i = static_cast<size_t>(std::stoul(n));
        if (i < model.animations.size()) return static_cast<int>(i);
    }
    return -1;
}

bool isAnimated(const ModelData& model, int anim) {
    return anim >= 0 && static_cast<size_t>(anim) < model.animations.size() && model.animations[static_cast<size_t>(anim)].duration > 0 &&
           !model.animations[static_cast<size_t>(anim)].channels.empty();
}

std::vector<Mat4> nodeWorlds(const ModelData& model, int anim, double time) {
    const size_t n = model.nodes.size();
    std::vector<std::array<float, 3>> t(n), s(n);
    std::vector<std::array<float, 4>> r(n);
    for (size_t i = 0; i < n; ++i) {
        t[i] = model.nodes[i].t;
        r[i] = model.nodes[i].r;
        s[i] = model.nodes[i].s;
    }
    if (anim >= 0 && static_cast<size_t>(anim) < model.animations.size()) {
        const ModelAnimation& a = model.animations[static_cast<size_t>(anim)];
        double tt = 0;
        if (a.duration > 0) tt = std::fmod(std::fmod(time, static_cast<double>(a.duration)) + a.duration, static_cast<double>(a.duration));
        for (const ModelChannel& c : a.channels) {
            if (c.node < 0 || static_cast<size_t>(c.node) >= n) continue;
            const size_t i = static_cast<size_t>(c.node);
            sampleChannel(c, static_cast<float>(tt), c.path == ChannelPath::Translation ? t[i].data() : c.path == ChannelPath::Rotation ? r[i].data() : s[i].data());
        }
    }
    std::vector<Mat4> worlds(n);
    std::vector<uint8_t> done(n, 0), busy(n, 0);
    const auto compute = [&](auto&& self, size_t i) -> const Mat4& {
        if (done[i]) return worlds[i];
        if (busy[i]) {   // a cycle in a broken file: treat the node as a root
            worlds[i] = identity();
            return worlds[i];
        }
        busy[i] = 1;
        const ModelNode& node = model.nodes[i];
        const Mat4 local = node.matrix ? *node.matrix : fromTrs(t[i], r[i], s[i]);
        worlds[i] = node.parent >= 0 && static_cast<size_t>(node.parent) < n ? multiply(self(self, static_cast<size_t>(node.parent)), local) : local;
        done[i] = 1;
        return worlds[i];
    };
    for (size_t i = 0; i < n; ++i) compute(compute, i);
    return worlds;
}

std::vector<PosedPrimitive> poseModel(const ModelData& model, int anim, double time, std::vector<PosedPrimitive>* out) {
    const std::vector<Mat4> worlds = nodeWorlds(model, anim, time);
    std::vector<std::vector<float>> skinMats(model.skins.size());
    std::vector<uint8_t> skinDone(model.skins.size(), 0);
    const auto jointsOf = [&](size_t skin) -> const std::vector<float>& {
        if (skinDone[skin]) return skinMats[skin];
        const ModelSkin& sk = model.skins[skin];
        std::vector<float>& m = skinMats[skin];
        m.assign(sk.joints.size() * 16, 0.0f);
        for (size_t k = 0; k < sk.joints.size(); ++k) {
            const int j = sk.joints[k];
            const Mat4 w = j >= 0 && static_cast<size_t>(j) < worlds.size() ? worlds[static_cast<size_t>(j)] : identity();
            Mat4 ib = identity();
            if (sk.inverseBind.size() >= k * 16 + 16) std::copy(sk.inverseBind.begin() + static_cast<std::ptrdiff_t>(k * 16), sk.inverseBind.begin() + static_cast<std::ptrdiff_t>(k * 16 + 16), ib.begin());
            const Mat4 jm = multiply(w, ib);
            std::copy(jm.begin(), jm.end(), m.begin() + static_cast<std::ptrdiff_t>(k * 16));
        }
        skinDone[skin] = 1;
        return m;
    };
    std::vector<PosedPrimitive> result;
    result.reserve(model.primitives.size());
    for (size_t pi = 0; pi < model.primitives.size(); ++pi) {
        const ModelPrimitive& p = model.primitives[pi];
        PosedPrimitive o;
        if (out && pi < out->size()) o = std::move((*out)[pi]);
        o.positions.resize(p.positions.size());
        o.normals.resize(p.normals.size());
        const std::vector<float>& P = p.positions;
        const std::vector<float>& N = p.normals;
        const size_t vertices = P.size() / 3;
        const bool skinned = p.skin >= 0 && static_cast<size_t>(p.skin) < model.skins.size() && p.joints.size() >= vertices * 4 && p.weights.size() >= vertices * 4;
        if (skinned) {
            const std::vector<float>& J = jointsOf(static_cast<size_t>(p.skin));
            const size_t jointCount = J.size() / 16;
            for (size_t v = 0; v < vertices; ++v) {
                float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
                const float px = P[v * 3], py = P[v * 3 + 1], pz = P[v * 3 + 2], qx = N[v * 3], qy = N[v * 3 + 1], qz = N[v * 3 + 2];
                for (size_t k = 0; k < 4; ++k) {
                    const float w = p.weights[v * 4 + k];
                    if (w == 0) continue;
                    const size_t ji = p.joints[v * 4 + k];
                    if (ji >= jointCount) continue;
                    const float* m = &J[ji * 16];
                    x += w * (m[0] * px + m[4] * py + m[8] * pz + m[12]);
                    y += w * (m[1] * px + m[5] * py + m[9] * pz + m[13]);
                    z += w * (m[2] * px + m[6] * py + m[10] * pz + m[14]);
                    nx += w * (m[0] * qx + m[4] * qy + m[8] * qz);
                    ny += w * (m[1] * qx + m[5] * qy + m[9] * qz);
                    nz += w * (m[2] * qx + m[6] * qy + m[10] * qz);
                }
                const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                const float inv = l > 0 ? 1 / l : 1;
                o.positions[v * 3] = x;
                o.positions[v * 3 + 1] = y;
                o.positions[v * 3 + 2] = z;
                o.normals[v * 3] = nx * inv;
                o.normals[v * 3 + 1] = ny * inv;
                o.normals[v * 3 + 2] = nz * inv;
            }
        } else {
            const Mat4 M = p.node >= 0 && static_cast<size_t>(p.node) < worlds.size() ? worlds[static_cast<size_t>(p.node)] : identity();
            for (size_t v = 0; v < vertices; ++v) {
                const float px = P[v * 3], py = P[v * 3 + 1], pz = P[v * 3 + 2], qx = N[v * 3], qy = N[v * 3 + 1], qz = N[v * 3 + 2];
                o.positions[v * 3] = M[0] * px + M[4] * py + M[8] * pz + M[12];
                o.positions[v * 3 + 1] = M[1] * px + M[5] * py + M[9] * pz + M[13];
                o.positions[v * 3 + 2] = M[2] * px + M[6] * py + M[10] * pz + M[14];
                const float nx = M[0] * qx + M[4] * qy + M[8] * qz, ny = M[1] * qx + M[5] * qy + M[9] * qz, nz = M[2] * qx + M[6] * qy + M[10] * qz;
                const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                const float inv = l > 0 ? 1 / l : 1;
                o.normals[v * 3] = nx * inv;
                o.normals[v * 3 + 1] = ny * inv;
                o.normals[v * 3 + 2] = nz * inv;
            }
        }
        result.push_back(std::move(o));
    }
    if (out) *out = result;
    return result;
}

Box boundsOfPose(const std::vector<PosedPrimitive>& posed) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    V3 mn{inf, inf, inf}, mx{-inf, -inf, -inf};
    for (const PosedPrimitive& p : posed)
        for (size_t i = 0; i + 2 < p.positions.size(); i += 3)
            for (size_t k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], static_cast<double>(p.positions[i + k]));
                mx[k] = std::max(mx[k], static_cast<double>(p.positions[i + k]));
            }
    if (mn[0] == inf) return {};
    return {mn, mx};
}

Mat4 modelMatrix(const V3& origin, const V3& anglesDeg, const V3& scale) {
    const Mat3 r = rotationFromAngles(anglesDeg);   // row major
    Mat4 m{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t row = 0; row < 3; ++row) m[c * 4 + row] = static_cast<float>(r[row * 3 + c] * scale[c]);
    m[12] = static_cast<float>(origin[0]);
    m[13] = static_cast<float>(origin[1]);
    m[14] = static_cast<float>(origin[2]);
    m[15] = 1;
    return m;
}

std::vector<float> vertexNormals(const std::vector<float>& pos, const std::vector<uint32_t>& idx) {
    std::vector<float> n(pos.size(), 0.0f);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const size_t a = idx[i] * 3, b = idx[i + 1] * 3, c = idx[i + 2] * 3;
        if (std::max({a, b, c}) + 2 >= pos.size()) continue;
        const float ux = pos[b] - pos[a], uy = pos[b + 1] - pos[a + 1], uz = pos[b + 2] - pos[a + 2];
        const float vx = pos[c] - pos[a], vy = pos[c + 1] - pos[a + 1], vz = pos[c + 2] - pos[a + 2];
        const float x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
        for (const size_t o : {a, b, c}) {
            n[o] += x;
            n[o + 1] += y;
            n[o + 2] += z;
        }
    }
    for (size_t i = 0; i + 2 < n.size(); i += 3) {
        const float l = std::sqrt(n[i] * n[i] + n[i + 1] * n[i + 1] + n[i + 2] * n[i + 2]);
        const float inv = l > 0 ? 1 / l : 1;
        n[i] *= inv;
        n[i + 1] *= inv;
        n[i + 2] *= inv;
    }
    return n;
}

} // namespace native::scene
