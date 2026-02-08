/**
 * Vulkan Capture — GPU-side frame capture into staging buffers.
 *
 * Copies the swapchain image into a GPU buffer that can be
 * fed into the interpolation pipeline.
 */

#pragma once

#include "../framegen_types.h"
#include <vector>
#include <mutex>

namespace framegen {

class VulkanCapture {
public:
    VulkanCapture() = default;
    ~VulkanCapture();

    // Initialize with the Vulkan device context
    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t queueFamilyIndex, uint32_t width, uint32_t height,
              VkFormat format);

    void shutdown();

    // Capture a swapchain image into our ring buffer
    // Returns the FrameData for the captured frame
    FrameData captureFrame(VkQueue queue, VkImage swapchainImage,
                           VkImageLayout currentLayout, uint64_t frameIndex);

    // Get the last N captured frames (for interpolation input)
    std::pair<FrameData, FrameData> getLastTwoFrames() const;

    // Ring buffer management
    uint32_t getBufferCount() const { return bufferCount_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width_ = 0, height_ = 0;
    uint32_t bufferCount_ = 4;

    // Ring buffer of GPU images
    struct CaptureBuffer {
        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint64_t frameIndex = 0;
        uint64_t timestampNs = 0;
        bool ready = false;
    };

    std::vector<CaptureBuffer> buffers_;
    uint32_t currentIndex_ = 0;
    mutable std::mutex mutex_;

    // Helpers
    bool createBuffer(CaptureBuffer& buf);
    void destroyBuffer(CaptureBuffer& buf);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                               VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
};

} // namespace framegen
