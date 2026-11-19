#version 460
// The level's sky behind everything: one triangle at the far plane, the view direction of every pixel from the inverse view-projection.
layout(push_constant) uniform PC {
    mat4 invViewProj;
    vec4 eye;
    vec4 misc;    // x = 3, y = the sky texture index
} pc;

layout(location = 0) out vec3 vN;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vTex;
layout(location = 3) flat out uint vFlags;
layout(location = 4) out vec3 vDir;

void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;   // (-1,-1) (3,-1) (-1,3)
    gl_Position = vec4(c, 1.0, 1.0);
    vec4 p = pc.invViewProj * vec4(c, 1.0, 1.0);
    vN = vec3(0.0, 1.0, 0.0);
    vUv = vec2(0.0);
    vTex = uint(pc.misc.y);
    vFlags = 1u;
    vDir = p.xyz / p.w - pc.eye.xyz;
}
