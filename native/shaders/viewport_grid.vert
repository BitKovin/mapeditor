#version 460
// Ground / axis grid of a viewport (T1.4), generated from the vertex index: no vertex buffer.
//   misc.x = 0: grid lines (2 * HALF + 1 lines per direction), 1: the three world axes (6 vertices)
//   grid.x = spacing (m), grid.y = plane (0 XZ, 1 XY, 2 ZY), grid.zw = centre in the plane (a, b)
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 grid;
    vec4 misc;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vFade;

const int HALF = 100;

vec3 planePoint(float a, float b) {
    int plane = int(pc.grid.y + 0.5);
    if (plane == 1) return vec3(a, b, 0.0);
    if (plane == 2) return vec3(0.0, b, a);
    return vec3(a, 0.0, b);
}

void main() {
    if (pc.misc.x > 0.5) {
        int line = gl_VertexIndex / 2;
        int end = gl_VertexIndex & 1;
        vec3 dir = line == 0 ? vec3(1, 0, 0) : (line == 1 ? vec3(0, 1, 0) : vec3(0, 0, 1));
        vec3 col = line == 0 ? vec3(0.95, 0.25, 0.25) : (line == 1 ? vec3(0.35, 0.85, 0.30) : vec3(0.30, 0.45, 1.0));
        gl_Position = pc.viewProj * vec4(dir * (end == 1 ? 2.0 : 0.0), 1.0);
        vColor = vec4(col, 1.0);
        vFade = vec2(0.0);
        return;
    }
    float spacing = pc.grid.x;
    int line = gl_VertexIndex / 2;
    int end = gl_VertexIndex & 1;
    int count = 2 * HALF + 1;
    int dirIndex = line / count;                     // 0: lines running along a, 1: along b
    int k = line % count - HALF;
    float ca = round(pc.grid.z / spacing), cb = round(pc.grid.w / spacing);
    float halfLen = float(HALF) * spacing;
    float a, b;
    int idx;                                         // grid index of the line across its own axis
    if (dirIndex == 0) {
        float bi = cb + float(k);
        idx = int(bi);
        a = ca * spacing + (end == 0 ? -halfLen : halfLen);
        b = bi * spacing;
    } else {
        float ai = ca + float(k);
        idx = int(ai);
        a = ai * spacing;
        b = cb * spacing + (end == 0 ? -halfLen : halfLen);
    }
    int plane = int(pc.grid.y + 0.5);
    vec3 c = vec3(0.34, 0.34, 0.37);                 // minor
    if (idx % 10 == 0) c = vec3(0.46, 0.46, 0.50);   // every tenth line
    if (idx == 0) {                                  // the axes through the origin
        if (dirIndex == 0) c = plane == 2 ? vec3(0.30, 0.45, 1.0) : vec3(0.95, 0.25, 0.25);
        else c = plane == 0 ? vec3(0.30, 0.45, 1.0) : vec3(0.35, 0.85, 0.30);
    }
    gl_Position = pc.viewProj * vec4(planePoint(a, b), 1.0);
    vColor = vec4(c, 1.0);
    vFade = vec2(a - ca * spacing, b - cb * spacing) / halfLen;
}
