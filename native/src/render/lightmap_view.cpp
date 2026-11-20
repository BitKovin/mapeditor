// render/lightmap_view -- see lightmap_view.h.
#include "render/lightmap_view.h"

namespace native::render {

LightmapView::LightmapView(const gpu::Device& device) : device_(device) {
    const VkDevice dev = device.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return;
    const VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &layout_) != VK_SUCCESS) return;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &pool_) != VK_SUCCESS) return;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(dev, &ai, &set_) != VK_SUCCESS) set_ = VK_NULL_HANDLE;
}

LightmapView::~LightmapView() {
    const VkDevice dev = device_.device();
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(dev, layout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
}

void LightmapView::update(VkImageView view) {
    if (!set_) return;
    VkDescriptorImageInfo ii{sampler_, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
}

} // namespace native::render
