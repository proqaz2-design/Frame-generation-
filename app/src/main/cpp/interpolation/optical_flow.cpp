/**
 * Optical Flow implementation
 */

#include "optical_flow.h"

namespace framegen {

OpticalFlow::~OpticalFlow() {
    shutdown();
}

bool OpticalFlow::init(VulkanCompute* compute, uint32_t width, uint32_t height) {
    compute_ = compute;
    width_ = width;
    height_ = height;

    // Create flow images
    if (!createFlowImage(forwardFlow_, VK_FORMAT_R16G16_SFLOAT, width, height)) return false;
    if (!createFlowImage(backwardFlow_, VK_FORMAT_R16G16_SFLOAT, width, height)) return false;
    if (!createFlowImage(confidenceMap_, VK_FORMAT_R16_SFLOAT, width, height)) return false;
    if (!createFlowImage(grayscale1_, VK_FORMAT_R16_SFLOAT, width, height)) return false;
    if (!createFlowImage(grayscale2_, VK_FORMAT_R16_SFLOAT, width, height)) return false;

    LOGI("OpticalFlow: Initialized %ux%u bidirectional flow", width, height);
    return true;
}

void OpticalFlow::shutdown() {
    if (!compute_) return;

    destroyFlowImage(forwardFlow_);
    destroyFlowImage(backwardFlow_);
    destroyFlowImage(confidenceMap_);
    destroyFlowImage(grayscale1_);
    destroyFlowImage(grayscale2_);

    compute_ = nullptr;
}

OpticalFlow::FlowResult OpticalFlow::computeBidirectional(
    const FrameData& frame1, const FrameData& frame2, VkSemaphore waitSem)
{
    auto startTime = Clock::now();

    VkCommandBuffer cmd = compute_->beginCompute();

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    // Step 1: RGB to Grayscale (Luma = 0.299R + 0.587G + 0.114B)
    {
        VulkanCompute::DispatchInfo info;
        info.pipelineName = "rgb_to_gray";
        info.groupCountX = (width_ + 15) / 16;
        info.groupCountY = (height_ + 15) / 16;
        info.groupCountZ = 1;
        compute_->dispatch(cmd, info);
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Step 2: Forward flow (frame1 -> frame2)
    {
        struct FlowPC {
            uint32_t width;
            uint32_t height;
            uint32_t searchRadius;
            uint32_t blockSize;
            float direction;  // 1.0 = forward, -1.0 = backward
            float pad[3];
        } pc = {width_, height_, 16, 8, 1.0f, {0, 0, 0}};

        VulkanCompute::DispatchInfo info;
        info.pipelineName = "block_match";
        info.groupCountX = (width_ + 7) / 8;
        info.groupCountY = (height_ + 7) / 8;
        info.groupCountZ = 1;
        info.pushConstants = &pc;
        info.pushConstantSize = sizeof(pc);
        compute_->dispatch(cmd, info);
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Step 3: Backward flow (frame2 -> frame1)
    {
        struct FlowPC {
            uint32_t width;
            uint32_t height;
            uint32_t searchRadius;
            uint32_t blockSize;
            float direction;
            float pad[3];
        } pc = {width_, height_, 16, 8, -1.0f, {0, 0, 0}};

        VulkanCompute::DispatchInfo info;
        info.pipelineName = "block_match";
        info.groupCountX = (width_ + 7) / 8;
        info.groupCountY = (height_ + 7) / 8;
        info.groupCountZ = 1;
        info.pushConstants = &pc;
        info.pushConstantSize = sizeof(pc);
        compute_->dispatch(cmd, info);
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Step 4: Forward-backward consistency check
    // If |F(x) + B(x + F(x))| > threshold, mark as occluded
    {
        struct ConsistencyPC {
            uint32_t width;
            uint32_t height;
            float threshold;  // Typically 1-2 pixels
            float pad;
        } pc = {width_, height_, 1.5f, 0.0f};

        VulkanCompute::DispatchInfo info;
        info.pipelineName = "flow_consistency";
        info.groupCountX = (width_ + 15) / 16;
        info.groupCountY = (height_ + 15) / 16;
        info.groupCountZ = 1;
        info.pushConstants = &pc;
        info.pushConstantSize = sizeof(pc);
        compute_->dispatch(cmd, info);
    }

    VkSemaphore doneSem = compute_->endComputeAndSubmit(cmd, waitSem);
    (void)doneSem;

    auto endTime = Clock::now();
    float elapsed = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    FlowResult result;
    result.forwardFlow = forwardFlow_.image;
    result.backwardFlow = backwardFlow_.image;
    result.confidenceMap = confidenceMap_.image;
    result.forwardFlowView = forwardFlow_.view;
    result.backwardFlowView = backwardFlow_.view;
    result.confidenceView = confidenceMap_.view;
    result.executionTimeMs = elapsed;

    return result;
}

bool OpticalFlow::createFlowImage(FlowImages& img, VkFormat format,
                                   uint32_t width, uint32_t height) {
    VkDevice dev = compute_->getDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &imageInfo, nullptr, &img.image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(dev, img.image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // Memory type index 0 is typically device local
    allocInfo.memoryTypeIndex = 0;

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, img.image, img.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    return vkCreateImageView(dev, &viewInfo, nullptr, &img.view) == VK_SUCCESS;
}

void OpticalFlow::destroyFlowImage(FlowImages& img) {
    VkDevice dev = compute_->getDevice();
    if (img.view != VK_NULL_HANDLE) vkDestroyImageView(dev, img.view, nullptr);
    if (img.image != VK_NULL_HANDLE) vkDestroyImage(dev, img.image, nullptr);
    if (img.memory != VK_NULL_HANDLE) vkFreeMemory(dev, img.memory, nullptr);
    img = {};
}

} // namespace framegen
