#include "light/mailbox.h"

#include <memory>

#include "native_shaders/scatter_comp.spv.h"

namespace native::light {

namespace {

VkSemaphore makeTimeline(VkDevice device) {
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    ti.initialValue = 0;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    VkSemaphore s = VK_NULL_HANDLE;
    return vkCreateSemaphore(device, &si, nullptr, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

} // namespace

Mailbox::Mailbox(gpu::Device& device, uint32_t entriesPerSlot, uint32_t slots) : device_(device), entriesPerSlot_(entriesPerSlot) {
    slots_.resize(slots);
    bool ok = slots > 0 && entriesPerSlot > 0;
    for (uint32_t i = 0; i < slots && ok; ++i) {
        slots_[i].buffer = std::make_unique<gpu::Arena>(device, static_cast<uint64_t>(entriesPerSlot) * sizeof(MailboxEntry), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                        "light mailbox " + std::to_string(i));
        ok = slots_[i].buffer->buffer() != VK_NULL_HANDLE;
    }
    bakeTimeline_ = makeTimeline(device.device());
    renderTimeline_ = makeTimeline(device.device());
    valid_ = ok && bakeTimeline_ != VK_NULL_HANDLE && renderTimeline_ != VK_NULL_HANDLE;
}

Mailbox::~Mailbox() {
    if (bakeTimeline_) vkDestroySemaphore(device_.device(), bakeTimeline_, nullptr);
    if (renderTimeline_) vkDestroySemaphore(device_.device(), renderTimeline_, nullptr);
}

uint64_t Mailbox::completedBake() const {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(device_.device(), bakeTimeline_, &v);
    return v;
}

uint64_t Mailbox::completedRender() const {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(device_.device(), renderTimeline_, &v);
    return v;
}

int Mailbox::acquire() {
    const uint64_t renderDone = completedRender();
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.state == State::Consumed && s.renderValue <= renderDone) s.state = State::Free;
        if (s.state == State::Free) {
            s.state = State::Composing;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Mailbox::commit(int slot, uint32_t entries, uint64_t bakeValue) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& s = slots_[static_cast<size_t>(slot)];
    s.state = State::Committed;
    s.entries = entries;
    s.bakeValue = bakeValue;
}

void Mailbox::abandon(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[static_cast<size_t>(slot)].state = State::Free;
}

uint64_t Mailbox::consumedBakeValue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consumedBake_;
}

Mailbox::Taken Mailbox::takeNewest() {
    const uint64_t bakeDone = completedBake();
    std::lock_guard<std::mutex> lock(mutex_);
    int best = -1;
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].state == State::Committed && slots_[i].bakeValue <= bakeDone && (best < 0 || slots_[i].bakeValue > slots_[static_cast<size_t>(best)].bakeValue))
            best = static_cast<int>(i);
    Taken t;
    if (best < 0) return t;
    // older finished mailboxes are covered by the newest one
    for (size_t i = 0; i < slots_.size(); ++i)
        if (static_cast<int>(i) != best && slots_[i].state == State::Committed && slots_[i].bakeValue < slots_[static_cast<size_t>(best)].bakeValue) slots_[i].state = State::Free;
    t.slot = best;
    t.entries = slots_[static_cast<size_t>(best)].entries;
    t.bakeValue = slots_[static_cast<size_t>(best)].bakeValue;
    return t;
}

void Mailbox::consumed(int slot, uint64_t renderValue) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& s = slots_[static_cast<size_t>(slot)];
    s.state = State::Consumed;
    s.renderValue = renderValue;
    if (s.bakeValue > consumedBake_) consumedBake_ = s.bakeValue;
}

// ---- scatter -----------------------------------------------------------------------------------------------------------------------

Scatter::Scatter(gpu::Device& device) : device_(device) {
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(device.device(), &li, nullptr, &setLayout_) != VK_SUCCESS) return;
    const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device.device(), &pl, nullptr, &layout_) != VK_SUCCESS) return;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(shaders::k_scatter_comp_spv);
    mi.pCode = shaders::k_scatter_comp_spv;
    if (vkCreateShaderModule(device.device(), &mi, nullptr, &module_) != VK_SUCCESS) return;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module_, "main", nullptr};
    pi.layout = layout_;
    if (vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_) != VK_SUCCESS) return;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 16;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    valid_ = vkCreateDescriptorPool(device.device(), &dpi, nullptr, &pool_) == VK_SUCCESS;
}

Scatter::~Scatter() {
    vkDeviceWaitIdle(device_.device());
    if (pool_) vkDestroyDescriptorPool(device_.device(), pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_.device(), pipeline_, nullptr);
    if (module_) vkDestroyShaderModule(device_.device(), module_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_.device(), setLayout_, nullptr);
}

void Scatter::resetDescriptors() {
    if (pool_) vkResetDescriptorPool(device_.device(), pool_, 0);
}

bool Scatter::record(VkCommandBuffer cmd, VkBuffer mailbox, uint32_t entries, VkImageView display) {
    if (!valid_ || mailbox == VK_NULL_HANDLE || display == VK_NULL_HANDLE) return false;
    if (entries == 0) return true;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_.device(), &ai, &set) != VK_SUCCESS) return false;
    const VkDescriptorBufferInfo bi{mailbox, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo ii{VK_NULL_HANDLE, display, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr}};
    vkUpdateDescriptorSets(device_.device(), 2, w, 0, nullptr);
    const uint32_t pc[4] = {entries, 0, 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, (entries + 63) / 64, 1, 1);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

} // namespace native::light
