#version 460
// Test triangle of T1.3: three hard-coded vertices, no vertex buffer. Replaced by the real passes of T3.x.
layout(location = 0) out vec3 vColor;

void main() {
    const vec2 pos[3] = vec2[3](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    const vec3 col[3] = vec3[3](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.3, 0.4, 1.0));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vColor = col[gl_VertexIndex];
}
