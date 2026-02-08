/**
 * GPU Buffer implementation
 */

#include "gpu_buffer.h"

namespace framegen {

GpuBuffer::~GpuBuffer() {
    destroy();
}

bool GpuBuffer::create(VkDevice device, VkPhysicalDevice physicalDevice,
                        VkDeviceSize size, Type type, VkBufferUsageFlags extraUsage) {
    device_ = device;
    size_ = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkMemoryPropertyFlags memFlags;

    switch (type) {
        case Type::STAGING:
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage;
            memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;

        case Type::DEVICE:
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage;
            memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;

        case Type::UNIFORM:
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | extraUsage;
            memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
    }

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        LOGE("GpuBuffer: Failed to create buffer (%lu bytes)", (unsigned long)size);
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, buffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, memFlags);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &memory_) != VK_SUCCESS) {
        LOGE("GpuBuffer: Failed to allocate memory");
        return false;
    }

    vkBindBufferMemory(device_, buffer_, memory_, 0);
    return true;
}

void GpuBuffer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    if (mapped_) {
        unmap();
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
}

void* GpuBuffer::map() {
    if (mapped_) return mapped_;

    vkMapMemory(device_, memory_, 0, size_, 0, &mapped_);
    return mapped_;
}

void GpuBuffer::unmap() {
    if (!mapped_) return;

    vkUnmapMemory(device_, memory_);
    mapped_ = nullptr;
}

void GpuBuffer::flush(VkDeviceSize offset, VkDeviceSize size) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory_;
    range.offset = offset;
    range.size = size;
    vkFlushMappedMemoryRanges(device_, 1, &range);
}

uint32_t GpuBuffer::findMemoryType(VkPhysicalDevice physicalDevice,
                                    uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

} // namespace framegen
