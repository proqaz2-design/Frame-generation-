/**
 * Vulkan Compute implementation
 */

#include "vulkan_compute.h"
#include <fstream>

namespace framegen {

VulkanCompute::~VulkanCompute() {
    shutdown();
}

bool VulkanCompute::init(VkDevice device, VkPhysicalDevice physicalDevice,
                          uint32_t computeQueueFamilyIndex) {
    device_ = device;
    physicalDevice_ = physicalDevice;

    vkGetDeviceQueue(device_, computeQueueFamilyIndex, 0, &computeQueue_);

    // Command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = computeQueueFamilyIndex;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        LOGE("VulkanCompute: Failed to create command pool");
        return false;
    }

    // Descriptor pool — large enough for all our compute work
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    };

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = 128;
    descPoolInfo.poolSizeCount = 4;
    descPoolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device_, &descPoolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOGE("VulkanCompute: Failed to create descriptor pool");
        return false;
    }

    // Pre-allocate semaphore pool
    semaphorePool_.resize(16);
    for (auto& sem : semaphorePool_) {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semInfo, nullptr, &sem);
    }

    LOGI("VulkanCompute: Initialized compute pipeline");
    return true;
}

void VulkanCompute::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);

    for (auto& [name, pipeline] : pipelines_) {
        if (pipeline.pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
        if (pipeline.pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipeline.pipelineLayout, nullptr);
        if (pipeline.descriptorSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, pipeline.descriptorSetLayout, nullptr);
        if (pipeline.shaderModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, pipeline.shaderModule, nullptr);
    }
    pipelines_.clear();

    for (auto& sem : semaphorePool_) {
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(device_, sem, nullptr);
    }

    if (descriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (commandPool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device_, commandPool_, nullptr);

    device_ = VK_NULL_HANDLE;
}

bool VulkanCompute::loadShader(const std::string& name, const uint32_t* spirvCode, size_t codeSize) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = spirvCode;

    PipelineData& pd = pipelines_[name];
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &pd.shaderModule) != VK_SUCCESS) {
        LOGE("VulkanCompute: Failed to create shader module: %s", name.c_str());
        return false;
    }
    return true;
}

bool VulkanCompute::loadShaderFromFile(const std::string& name, const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        LOGE("VulkanCompute: Could not open shader: %s", path.c_str());
        return false;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), fileSize);
    file.close();

    return loadShader(name, code.data(), fileSize);
}

bool VulkanCompute::createPipeline(const std::string& shaderName,
                                    const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    auto it = pipelines_.find(shaderName);
    if (it == pipelines_.end()) {
        LOGE("VulkanCompute: Shader not loaded: %s", shaderName.c_str());
        return false;
    }

    PipelineData& pd = it->second;

    // Descriptor set layout
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &pd.descriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    // Push constant range (16 floats = 64 bytes for params)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 64;

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &pd.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pd.pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = pd.shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pd.pipelineLayout;

    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pd.pipeline) != VK_SUCCESS) {
        LOGE("VulkanCompute: Failed to create compute pipeline: %s", shaderName.c_str());
        return false;
    }

    LOGI("VulkanCompute: Pipeline created: %s", shaderName.c_str());
    return true;
}

VkCommandBuffer VulkanCompute::beginCompute() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    return cmd;
}

void VulkanCompute::dispatch(VkCommandBuffer cmd, const DispatchInfo& info) {
    auto it = pipelines_.find(info.pipelineName);
    if (it == pipelines_.end()) {
        LOGE("VulkanCompute: Pipeline not found: %s", info.pipelineName.c_str());
        return;
    }

    PipelineData& pd = it->second;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pd.pipeline);

    if (!info.descriptorSets.empty()) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            pd.pipelineLayout, 0,
            static_cast<uint32_t>(info.descriptorSets.size()),
            info.descriptorSets.data(), 0, nullptr);
    }

    if (info.pushConstants && info.pushConstantSize > 0) {
        vkCmdPushConstants(cmd, pd.pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0,
            info.pushConstantSize, info.pushConstants);
    }

    vkCmdDispatch(cmd, info.groupCountX, info.groupCountY, info.groupCountZ);
}

VkSemaphore VulkanCompute::endComputeAndSubmit(VkCommandBuffer cmd, VkSemaphore waitSemaphore) {
    vkEndCommandBuffer(cmd);

    VkSemaphore signalSem = getNextSemaphore();

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSem;

    if (waitSemaphore != VK_NULL_HANDLE) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
    }

    vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE);

    return signalSem;
}

VkDescriptorSet VulkanCompute::allocateDescriptorSet(const std::string& pipelineName) {
    auto it = pipelines_.find(pipelineName);
    if (it == pipelines_.end()) return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &it->second.descriptorSetLayout;

    VkDescriptorSet set;
    vkAllocateDescriptorSets(device_, &allocInfo, &set);
    return set;
}

void VulkanCompute::updateDescriptorImage(VkDescriptorSet set, uint32_t binding,
                                           VkImageView imageView, VkSampler sampler,
                                           VkImageLayout layout) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = layout;
    imageInfo.imageView = imageView;
    imageInfo.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanCompute::updateDescriptorStorageImage(VkDescriptorSet set, uint32_t binding,
                                                  VkImageView imageView) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfo.imageView = imageView;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanCompute::updateDescriptorBuffer(VkDescriptorSet set, uint32_t binding,
                                            VkBuffer buffer, VkDeviceSize size) {
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = buffer;
    bufInfo.offset = 0;
    bufInfo.range = size;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

VkSemaphore VulkanCompute::getNextSemaphore() {
    VkSemaphore sem = semaphorePool_[semaphoreIndex_];
    semaphoreIndex_ = (semaphoreIndex_ + 1) % semaphorePool_.size();
    return sem;
}

} // namespace framegen
