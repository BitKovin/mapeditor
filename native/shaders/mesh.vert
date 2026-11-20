#version 460
// The scene's triangles: positions, octahedral normals, albedo uv, flags and the global texture index straight from the vertex arena
// (scene/render_mesh.h RenderVertex, 40 bytes). The same push-constant block as mesh.frag.
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;
    vec4 misc;    // x = pass (0 opaque, 1 translucent, 2 volume, 3 sky background), y = sky texture index (background)
    vec4 lit;     // x = mode (0 unlit, 1 lit, 2 lighting only), y = exposure, z = brightness, w = texture lod bias (fragment shader)
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in uint aPageFlags;   // low 16 bits: lightmap page, high 16 bits: flags
layout(location = 4) in uint aTex;
layout(location = 5) in vec2 aLm;          // lightmap coordinates of the live layout (RenderVertex::lm), page in aPageFlags

layout(location = 0) out vec3 vN;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vTex;
layout(location = 3) flat out uint vFlags;
layout(location = 4) out vec3 vDir;
layout(location = 5) out vec2 vLm;
layout(location = 6) flat out uint vPage;

vec3 octDecode(uint p) {
    vec2 e = vec2(float(int(p << 16) >> 16), float(int(p) >> 16)) / 32767.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main() {
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
    vN = octDecode(aNormal);
    vUv = aUv;
    vTex = aTex;
    vFlags = aPageFlags >> 16;
    vLm = aLm;
    vPage = aPageFlags & 0xffffu;
    vDir = aPos - pc.eye.xyz;
}
