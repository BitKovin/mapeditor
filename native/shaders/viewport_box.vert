#version 460
// The unit test box of T1.4: a 1 m cube standing on the origin, 36 vertices from the vertex index. Replaced by the real passes (T3.x).
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 grid;
    vec4 misc;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vFade;

// Per face: outward normal n and tangents u, v with u x v = n (counter-clockwise seen from outside).
const vec3 N[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 U[6] = vec3[6](vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(0, 1, 0));
const vec3 V[6] = vec3[6](vec3(0, 0, 1), vec3(0, 1, 0), vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 1, 0), vec3(1, 0, 0));
const vec2 Q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    int face = gl_VertexIndex / 6;
    vec2 q = Q[gl_VertexIndex % 6];
    vec3 p = vec3(0.0, 0.5, 0.0) + 0.5 * (N[face] + q.x * U[face] + q.y * V[face]);
    gl_Position = pc.viewProj * vec4(p, 1.0);
    float light = 0.55 + 0.45 * max(dot(N[face], normalize(vec3(0.4, 0.8, 0.5))), 0.0);
    vec3 base = vec3(0.55) + 0.35 * abs(N[face]) * vec3(0.9, 0.7, 0.5);
    vColor = vec4(base * light, 1.0);
    vFade = vec2(0.0);
}
