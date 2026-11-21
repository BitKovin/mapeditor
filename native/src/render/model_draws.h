// render/model_draws -- what the model pass draws in a viewport frame: one draw per primitive of a model, instanced over the entities that show it (buffers already in
// the GPU / the upload ring). Pure data; made by ModelRenderer::buildDraws, recorded by ModelPass.
// Thread: render thread.
// Spec: 08_MILESTONES.md T11.9c.
#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

namespace native::render {

struct ModelDraw {
    VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE, instanceBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0, indexOffset = 0, instanceOffset = 0;
    uint32_t indexCount = 0, instanceCount = 0;
    float color[4] = {1, 1, 1, 1};   // the material's base colour factor
    float texture = 0;               // the texture table index of the base colour image
    float hasTexture = 0;            // 1 = multiply by the texture
    float alphaMode = 0;             // 0 opaque, 1 cutout at `cutoff`
    float cutoff = 0.5f;
};

struct ModelDrawList {
    std::vector<ModelDraw> draws;
    uint32_t instances = 0;   // entities drawn (statistics)
};

// The vertex a model draw reads (binding 0) and the per-instance matrix (binding 1, a column-major mat4).
struct ModelVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(ModelVertex) == 32);
constexpr uint32_t kModelInstanceBytes = 64;

} // namespace native::render
