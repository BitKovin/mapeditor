#version 460
// The models of entities (render/model_pass): positions and normals of one primitive (already posed, model space) and the entity's model -> world matrix as an
// instance attribute. The push-constant block is the one of mesh.vert / mesh.frag (same pipeline layout); the fields are used differently:
//   color = the material's base colour factor, mat = (texture index, has texture, alpha mode 0 opaque / 1 cutout, cutoff), lit = as the mesh pass.
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 color;
    vec4 mat;
    vec4 lit;
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aM0;   // the columns of the instance's model matrix
layout(location = 4) in vec4 aM1;
layout(location = 5) in vec4 aM2;
layout(location = 6) in vec4 aM3;

layout(location = 0) out vec3 vN;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vLight;   // the entry of the light volume's value for this entity (0 = none): stored in the unused w of the first matrix column

void main() {
    vLight = uint(aM0.w + 0.5);
    mat4 m = mat4(vec4(aM0.xyz, 0.0), aM1, aM2, aM3);
    gl_Position = pc.viewProj * (m * vec4(aPos, 1.0));
    mat3 m3 = mat3(m);
    vN = transpose(inverse(m3)) * aNormal;   // right for non-uniform scales and mirrors too
    vUv = aUv;
}
