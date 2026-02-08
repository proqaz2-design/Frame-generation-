/**
 * GPU Buffer — Vulkan buffer abstraction for data transfers.
 */

#pragma once

#include "../framegen_types.h"

namespace framegen {

class GpuBuffer {
public:
    enum class Type {
        STAGING,    // CPU-visible, for transfers
        DEVICE,     // GPU-only, fastest
        UNIFORM,    // Small, frequently updated
    };

    GpuBuffer() = default;
    ~GpuBuffer();

    bool create(VkDevice device, VkPhysicalDevice physicalDevice,
                VkDeviceSize size, Type type,
                VkBufferUsageFlags extraUsage = 0);
    void destroy();

    void* map();
    void unmap();
    void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    VkBuffer buffer() const { return buffer_; }
    VkDeviceMemory memory() const { return memory_; }
    VkDeviceSize size() const { return size_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;

    uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                            uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

} // namespace framegen
