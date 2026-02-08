/**
 * Vulkan Layer — Intercepts vkQueuePresentKHR to capture game frames.
 *
 * This is an implicit Vulkan layer that gets loaded when a game runs
 * inside the FrameGen sandbox. It hooks into the present call to
 * extract the final rendered frame before it goes to the display.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <functional>

#ifndef VK_LAYER_EXPORT
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif
#endif

namespace framegen {

// Callback type: called with source image + dimensions when a frame is captured
using FrameCaptureCallback = std::function<void(
    VkDevice device,
    VkQueue queue,
    VkImage srcImage,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    uint64_t frameIndex
)>;

class VulkanLayer {
public:
    static VulkanLayer& instance();

    // Layer lifecycle
    VkResult onCreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance
    );

    void onDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);

    VkResult onCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice
    );

    void onDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);

    // The key hook — intercept frame presentation
    VkResult onQueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

    // Register callback for captured frames
    void setFrameCaptureCallback(FrameCaptureCallback callback);

    // Control
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    // Dispatch table management
    PFN_vkVoidFunction getDeviceProcAddr(VkDevice device, const char* pName);
    PFN_vkVoidFunction getInstanceProcAddr(VkInstance instance, const char* pName);

private:
    VulkanLayer() = default;

    struct DeviceData {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        uint32_t graphicsQueueFamilyIndex = 0;
        VkCommandPool commandPool = VK_NULL_HANDLE;

        // Original dispatch
        PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = nullptr;
        PFN_vkDestroyDevice fpDestroyDevice = nullptr;
        PFN_vkQueuePresentKHR fpQueuePresentKHR = nullptr;
        PFN_vkCreateCommandPool fpCreateCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers fpAllocateCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer fpBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer fpEndCommandBuffer = nullptr;
        PFN_vkCmdCopyImage fpCmdCopyImage = nullptr;
        PFN_vkCmdPipelineBarrier fpCmdPipelineBarrier = nullptr;
        PFN_vkQueueSubmit fpQueueSubmit = nullptr;
        PFN_vkQueueWaitIdle fpQueueWaitIdle = nullptr;
        PFN_vkFreeCommandBuffers fpFreeCommandBuffers = nullptr;
        PFN_vkCreateImage fpCreateImage = nullptr;
        PFN_vkDestroyImage fpDestroyImage = nullptr;
        PFN_vkAllocateMemory fpAllocateMemory = nullptr;
        PFN_vkFreeMemory fpFreeMemory = nullptr;
        PFN_vkBindImageMemory fpBindImageMemory = nullptr;
        PFN_vkGetImageMemoryRequirements fpGetImageMemoryRequirements = nullptr;

        // Swapchain info
        uint32_t swapchainWidth = 0;
        uint32_t swapchainHeight = 0;
        VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    };

    struct InstanceData {
        VkInstance instance = VK_NULL_HANDLE;
        PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = nullptr;
        PFN_vkDestroyInstance fpDestroyInstance = nullptr;
    };

    std::mutex mutex_;
    std::unordered_map<void*, DeviceData> devices_;
    std::unordered_map<void*, InstanceData> instances_;

    FrameCaptureCallback captureCallback_;
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> frameCounter_{0};

    // Helpers
    void* getKey(void* handle) { return *(void**)handle; }
    DeviceData& getDeviceData(void* key);
    InstanceData& getInstanceData(void* key);
};

} // namespace framegen

// ============================================================
// Vulkan Layer entry points (extern "C")
// ============================================================
extern "C" {

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance);

VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator);

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice);

VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator);

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_QueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo);

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetDeviceProcAddr(
    VkDevice device, const char* pName);

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetInstanceProcAddr(
    VkInstance instance, const char* pName);

// Layer enumeration
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties);

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pPropertyCount, VkLayerProperties* pProperties);

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties);

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties);

} // extern "C"
