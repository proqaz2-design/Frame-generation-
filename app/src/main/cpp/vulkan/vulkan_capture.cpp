/**
 * Vulkan Capture implementation — ring-buffer GPU frame copy
 */

#include "vulkan_capture.h"
#include <algorithm>

namespace framegen {

VulkanCapture::~VulkanCapture() {
    shutdown();
}

bool VulkanCapture::init(VkDevice device, VkPhysicalDevice physicalDevice,
                         uint32_t queueFamilyIndex, uint32_t width,
                         uint32_t height, VkFormat format) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    width_ = width;
    height_ = height;
    format_ = format;

    // Create command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        LOGE("VulkanCapture: Failed to create command pool");
        return false;
    }

    // Allocate ring buffer
    buffers_.resize(bufferCount_);
    for (auto& buf : buffers_) {
        if (!createBuffer(buf)) {
            LOGE("VulkanCapture: Failed to create capture buffer");
            return false;
        }
    }

    LOGI("VulkanCapture: Initialized %ux%u ring buffer (%u frames)", width_, height_, bufferCount_);
    return true;
}

void VulkanCapture::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);

    for (auto& buf : buffers_) {
        destroyBuffer(buf);
    }
    buffers_.clear();

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    device_ = VK_NULL_HANDLE;
}

bool VulkanCapture::createBuffer(CaptureBuffer& buf) {
    // Create image for frame storage
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format_;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &imageInfo, nullptr, &buf.image) != VK_SUCCESS) {
        return false;
    }

    // Allocate memory
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device_, buf.image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &buf.memory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(device_, buf.image, buf.memory, 0);

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = buf.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &buf.imageView) != VK_SUCCESS) {
        return false;
    }

    // Command buffer
    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;

    vkAllocateCommandBuffers(device_, &cmdInfo, &buf.cmdBuffer);

    // Fence
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fenceInfo, nullptr, &buf.fence);

    // Semaphore
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &semInfo, nullptr, &buf.semaphore);

    return true;
}

void VulkanCapture::destroyBuffer(CaptureBuffer& buf) {
    if (buf.semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(device_, buf.semaphore, nullptr);
    if (buf.fence != VK_NULL_HANDLE)
        vkDestroyFence(device_, buf.fence, nullptr);
    if (buf.imageView != VK_NULL_HANDLE)
        vkDestroyImageView(device_, buf.imageView, nullptr);
    if (buf.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, buf.image, nullptr);
    if (buf.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, buf.memory, nullptr);
}

FrameData VulkanCapture::captureFrame(VkQueue queue, VkImage swapchainImage,
                                       VkImageLayout currentLayout,
                                       uint64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);

    CaptureBuffer& buf = buffers_[currentIndex_];

    // Wait for previous use of this buffer to complete
    vkWaitForFences(device_, 1, &buf.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &buf.fence);

    // Record copy command
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkResetCommandBuffer(buf.cmdBuffer, 0);
    vkBeginCommandBuffer(buf.cmdBuffer, &beginInfo);

    // Transition swapchain image to transfer src
    transitionImageLayout(buf.cmdBuffer, swapchainImage,
        currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Transition our buffer to transfer dst
    transitionImageLayout(buf.cmdBuffer, buf.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Copy
    VkImageCopy copyRegion{};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.extent = {width_, height_, 1};

    vkCmdCopyImage(buf.cmdBuffer,
        swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buf.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copyRegion);

    // Transition our buffer to shader read (for the AI model)
    transitionImageLayout(buf.cmdBuffer, buf.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Transition swapchain image back
    transitionImageLayout(buf.cmdBuffer, swapchainImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(buf.cmdBuffer);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &buf.cmdBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &buf.semaphore;

    vkQueueSubmit(queue, 1, &submitInfo, buf.fence);

    // Record metadata
    buf.frameIndex = frameIndex;
    buf.timestampNs = now_ns();
    buf.ready = true;

    // Build FrameData
    FrameData frame;
    frame.image = buf.image;
    frame.image_view = buf.imageView;
    frame.memory = buf.memory;
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    frame.timestamp_ns = buf.timestampNs;
    frame.frame_index = frameIndex;
    frame.render_complete = buf.semaphore;
    frame.fence = buf.fence;
    frame.is_interpolated = false;

    // Advance ring buffer
    currentIndex_ = (currentIndex_ + 1) % bufferCount_;

    return frame;
}

std::pair<FrameData, FrameData> VulkanCapture::getLastTwoFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t prev = (currentIndex_ + bufferCount_ - 2) % bufferCount_;
    uint32_t curr = (currentIndex_ + bufferCount_ - 1) % bufferCount_;

    auto makeFrame = [&](const CaptureBuffer& buf) -> FrameData {
        FrameData f;
        f.image = buf.image;
        f.image_view = buf.imageView;
        f.memory = buf.memory;
        f.width = width_;
        f.height = height_;
        f.format = format_;
        f.timestamp_ns = buf.timestampNs;
        f.frame_index = buf.frameIndex;
        f.render_complete = buf.semaphore;
        f.fence = buf.fence;
        return f;
    };

    return {makeFrame(buffers_[prev]), makeFrame(buffers_[curr])};
}

uint32_t VulkanCapture::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    LOGE("VulkanCapture: Failed to find suitable memory type!");
    return 0;
}

void VulkanCapture::transitionImageLayout(
    VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(cmd,
        srcStage, dstStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);
}

} // namespace framegen
