#version 460
// Coloured lines: the selection outlines (world space) and the rubber band of a box select (screen space: the matrix is the identity).
layout(push_constant) uniform PC {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
    vColor = aColor;
}
