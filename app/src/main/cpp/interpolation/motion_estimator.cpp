/**
 * Motion Estimator implementation — hierarchical block matching on GPU
 */

#include "motion_estimator.h"

namespace framegen {

MotionEstimator::~MotionEstimator() {
    shutdown();
}

bool MotionEstimator::init(VulkanCompute* compute, uint32_t width, uint32_t height) {
    compute_ = compute;
    width_ = width;
    height_ = height;

    if (!createFlowField()) {
        LOGE("MotionEstimator: Failed to create flow field");
        return false;
    }

    if (!createPyramid()) {
        LOGE("MotionEstimator: Failed to create image pyramid");
        return false;
    }

    // Setup compute pipelines for each stage
    // 1. Downsample shader
    std::vector<VkDescriptorSetLayoutBinding> downsampleBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    // 2. Block matching shader
    std::vector<VkDescriptorSetLayoutBinding> matchBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // prev level flow
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},            // output flow
    };

    // 3. Flow refinement shader
    std::vector<VkDescriptorSetLayoutBinding> refineBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    LOGI("MotionEstimator: Initialized %ux%u, %u pyramid levels, block=%u, search=%u",
         width_, height_, pyramidLevels_, blockSize_, searchRadius_);
    return true;
}

void MotionEstimator::shutdown() {
    if (!compute_) return;

    VkDevice dev = compute_->getDevice();
    vkDeviceWaitIdle(dev);

    if (flowImageView_ != VK_NULL_HANDLE) vkDestroyImageView(dev, flowImageView_, nullptr);
    if (flowImage_ != VK_NULL_HANDLE) vkDestroyImage(dev, flowImage_, nullptr);
    if (flowMemory_ != VK_NULL_HANDLE) vkFreeMemory(dev, flowMemory_, nullptr);

    destroyPyramid();
    compute_ = nullptr;
}

float MotionEstimator::estimate(const FrameData& frame1, const FrameData& frame2,
                                 VkImage flowOut, VkSemaphore waitSem) {
    auto startTime = Clock::now();

    VkCommandBuffer cmd = compute_->beginCompute();

    // ===== Stage 1: Build image pyramids =====
    // Downsample both frames at each level
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    for (uint32_t level = 0; level < pyramidLevels_; level++) {
        uint32_t lw = pyramid_[level].width;
        uint32_t lh = pyramid_[level].height;

        VulkanCompute::DispatchInfo downsampleInfo;
        downsampleInfo.pipelineName = "downsample";
        downsampleInfo.groupCountX = (lw + 15) / 16;
        downsampleInfo.groupCountY = (lh + 15) / 16;
        downsampleInfo.groupCountZ = 1;

        compute_->dispatch(cmd, downsampleInfo);

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ===== Stage 2: Coarse-to-fine block matching =====
    // Start from the coarsest level and propagate upward
    for (int level = static_cast<int>(pyramidLevels_) - 1; level >= 0; level--) {
        uint32_t lw = pyramid_[level].width;
        uint32_t lh = pyramid_[level].height;

        struct MatchPushConstants {
            uint32_t width;
            uint32_t height;
            uint32_t blockSize;
            uint32_t searchRadius;
            uint32_t level;
            uint32_t totalLevels;
            float pad[2];
        } matchPC = {
            lw, lh, blockSize_, searchRadius_,
            static_cast<uint32_t>(level), pyramidLevels_, {0, 0}
        };

        VulkanCompute::DispatchInfo matchInfo;
        matchInfo.pipelineName = "block_match";
        matchInfo.groupCountX = (lw + blockSize_ - 1) / blockSize_;
        matchInfo.groupCountY = (lh + blockSize_ - 1) / blockSize_;
        matchInfo.groupCountZ = 1;
        matchInfo.pushConstants = &matchPC;
        matchInfo.pushConstantSize = sizeof(matchPC);

        compute_->dispatch(cmd, matchInfo);

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ===== Stage 3: Sub-pixel refinement at full resolution =====
    VulkanCompute::DispatchInfo refineInfo;
    refineInfo.pipelineName = "flow_refine";
    refineInfo.groupCountX = (width_ + 15) / 16;
    refineInfo.groupCountY = (height_ + 15) / 16;
    refineInfo.groupCountZ = 1;

    compute_->dispatch(cmd, refineInfo);

    // Submit
    VkSemaphore doneSem = compute_->endComputeAndSubmit(cmd, waitSem);
    (void)doneSem;

    auto endTime = Clock::now();
    float elapsed = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    LOGD("MotionEstimator: %.2f ms (%u levels)", elapsed, pyramidLevels_);
    return elapsed;
}

bool MotionEstimator::createFlowField() {
    VkDevice dev = compute_->getDevice();

    // RG16F format — 2 channels (dx, dy) in float16
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &imageInfo, nullptr, &flowImage_) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(dev, flowImage_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // Find device local memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(compute_->getPhysicalDevice(), &memProps);
    allocInfo.memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &flowMemory_) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(dev, flowImage_, flowMemory_, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = flowImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    return vkCreateImageView(dev, &viewInfo, nullptr, &flowImageView_) == VK_SUCCESS;
}

bool MotionEstimator::createPyramid() {
    pyramid_.resize(pyramidLevels_);

    uint32_t w = width_;
    uint32_t h = height_;

    for (uint32_t i = 0; i < pyramidLevels_; i++) {
        if (i > 0) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        pyramid_[i].width = w;
        pyramid_[i].height = h;

        // In a full implementation, create VkImage for each level
        // For now we track dimensions
        LOGD("Pyramid level %u: %ux%u", i, w, h);
    }

    return true;
}

void MotionEstimator::destroyPyramid() {
    VkDevice dev = compute_ ? compute_->getDevice() : VK_NULL_HANDLE;
    if (dev == VK_NULL_HANDLE) return;

    for (auto& level : pyramid_) {
        if (level.view1 != VK_NULL_HANDLE) vkDestroyImageView(dev, level.view1, nullptr);
        if (level.view2 != VK_NULL_HANDLE) vkDestroyImageView(dev, level.view2, nullptr);
        if (level.flowView != VK_NULL_HANDLE) vkDestroyImageView(dev, level.flowView, nullptr);
        if (level.image1 != VK_NULL_HANDLE) vkDestroyImage(dev, level.image1, nullptr);
        if (level.image2 != VK_NULL_HANDLE) vkDestroyImage(dev, level.image2, nullptr);
        if (level.flow != VK_NULL_HANDLE) vkDestroyImage(dev, level.flow, nullptr);
        if (level.mem1 != VK_NULL_HANDLE) vkFreeMemory(dev, level.mem1, nullptr);
        if (level.mem2 != VK_NULL_HANDLE) vkFreeMemory(dev, level.mem2, nullptr);
        if (level.flowMem != VK_NULL_HANDLE) vkFreeMemory(dev, level.flowMem, nullptr);
    }
    pyramid_.clear();
}

} // namespace framegen
