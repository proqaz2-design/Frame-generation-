/**
 * FrameGen Vulkan Layer — SELF-CONTAINED frame generation layer.
 *
 * This is an implicit Vulkan layer loaded into games via Android's
 * gpu_debug_layers mechanism. It does ALL work internally:
 *
 * 1. Hooks vkCreateSwapchainKHR → tracks swapchain images + creates staging
 * 2. Hooks vkQueuePresentKHR → captures frames + inserts interpolated frames
 * 3. Uses compute shaders for frame blending/interpolation
 *
 * The app only configures gpu_debug_layers via Shizuku.
 * This runs entirely inside the GAME's process.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <android/log.h>
#include <chrono>

#ifndef VK_LAYER_EXPORT
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif
#endif

#define FG_TAG "FrameGenLayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  FG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  FG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, FG_TAG, __VA_ARGS__)

// ============================================================
// Vulkan Layer dispatch types — not in standard NDK headers
// ============================================================
#ifndef VK_LAYER_LINK_INFO

typedef enum VkLayerFunction_ {
    VK_LAYER_LINK_INFO = 0,
    VK_LAYER_DEVICE_INFO = 1
} VkLayerFunction;

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
} VkLayerInstanceLink;

typedef struct VkLayerInstanceCreateInfo_ {
    VkStructureType sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerInstanceLink* pLayerInfo;
    } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct VkLayerDeviceCreateInfo_ {
    VkStructureType sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerDeviceLink* pLayerInfo;
    } u;
} VkLayerDeviceCreateInfo;

#endif // VK_LAYER_LINK_INFO

namespace framegen {

class VulkanLayer {
public:
    static VulkanLayer& instance();

    // ─── Lifecycle ──────────────────────────────────
    VkResult onCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
    void onDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);

    VkResult onCreateDevice(VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
    void onDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);

    // ─── Swapchain hooks ────────────────────────────
    VkResult onCreateSwapchain(VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
    void onDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* pAllocator);

    // ─── Frame generation on present ────────────────
    VkResult onQueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

    // ─── Dispatch ───────────────────────────────────
    PFN_vkVoidFunction getDeviceProcAddr(VkDevice device, const char* pName);
    PFN_vkVoidFunction getInstanceProcAddr(VkInstance instance, const char* pName);

    void setEnabled(bool e) { enabled_ = e; }

private:
    VulkanLayer() = default;

    // ─── Staging image for frame capture ────────────
    struct StagingImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool valid = false;
    };

    struct SwapchainData {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
    };

    struct DeviceData {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t graphicsFamily = 0;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;

        // Swapchain tracking
        std::unordered_map<uint64_t, SwapchainData> swapchains;

        // Frame capture: double-buffer staging
        StagingImage prevFrame;
        StagingImage curFrame;
        bool hasPrev = false;
        uint32_t captureW = 0, captureH = 0;
        VkFormat captureFormat = VK_FORMAT_UNDEFINED;

        // Performance
        uint64_t frameCount = 0;
        uint64_t interpCount = 0;

        // ─── Next-layer dispatch table ──────────────
        PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = nullptr;
        PFN_vkDestroyDevice fpDestroyDevice = nullptr;
        PFN_vkQueuePresentKHR fpQueuePresentKHR = nullptr;
        PFN_vkCreateSwapchainKHR fpCreateSwapchainKHR = nullptr;
        PFN_vkDestroySwapchainKHR fpDestroySwapchainKHR = nullptr;
        PFN_vkGetSwapchainImagesKHR fpGetSwapchainImagesKHR = nullptr;
        PFN_vkAcquireNextImageKHR fpAcquireNextImageKHR = nullptr;
        PFN_vkQueueSubmit fpQueueSubmit = nullptr;
        PFN_vkQueueWaitIdle fpQueueWaitIdle = nullptr;
        PFN_vkCreateCommandPool fpCreateCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers fpAllocateCommandBuffers = nullptr;
        PFN_vkFreeCommandBuffers fpFreeCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer fpBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer fpEndCommandBuffer = nullptr;
        PFN_vkCmdCopyImage fpCmdCopyImage = nullptr;
        PFN_vkCmdBlitImage fpCmdBlitImage = nullptr;
        PFN_vkCmdPipelineBarrier fpCmdPipelineBarrier = nullptr;
        PFN_vkCreateImage fpCreateImage = nullptr;
        PFN_vkDestroyImage fpDestroyImage = nullptr;
        PFN_vkAllocateMemory fpAllocateMemory = nullptr;
        PFN_vkFreeMemory fpFreeMemory = nullptr;
        PFN_vkBindImageMemory fpBindImageMemory = nullptr;
        PFN_vkGetImageMemoryRequirements fpGetImageMemoryRequirements = nullptr;
        PFN_vkCreateFence fpCreateFence = nullptr;
        PFN_vkDestroyFence fpDestroyFence = nullptr;
        PFN_vkWaitForFences fpWaitForFences = nullptr;
        PFN_vkResetFences fpResetFences = nullptr;
        PFN_vkCreateSemaphore fpCreateSemaphore = nullptr;
        PFN_vkDestroySemaphore fpDestroySemaphore = nullptr;
        PFN_vkResetCommandBuffer fpResetCommandBuffer = nullptr;
        PFN_vkDeviceWaitIdle fpDeviceWaitIdle = nullptr;
    };

    struct InstanceData {
        VkInstance instance = VK_NULL_HANDLE;
        PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = nullptr;
        PFN_vkDestroyInstance fpDestroyInstance = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties fpGetPhysMemProps = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties fpGetPhysQueueFamilyProps = nullptr;
    };

    // ─── Helpers ────────────────────────────────────
    void* getKey(void* handle) { return *(void**)handle; }
    DeviceData& getDeviceData(void* key);
    InstanceData& getInstanceData(void* key);

    bool createStagingImage(DeviceData& dev, StagingImage& img,
        uint32_t w, uint32_t h, VkFormat format);
    void destroyStagingImage(DeviceData& dev, StagingImage& img);
    void ensureStaging(DeviceData& dev, uint32_t w, uint32_t h, VkFormat fmt);

    uint32_t findMemoryType(DeviceData& dev, uint32_t filter,
        VkMemoryPropertyFlags props);

    void transitionImage(VkCommandBuffer cmd, DeviceData& dev, VkImage image,
        VkImageLayout oldL, VkImageLayout newL,
        VkAccessFlags srcA, VkAccessFlags dstA,
        VkPipelineStageFlags srcS, VkPipelineStageFlags dstS);

    void copyImage(VkCommandBuffer cmd, DeviceData& dev,
        VkImage src, VkImageLayout srcLayout,
        VkImage dst, VkImageLayout dstLayout,
        uint32_t w, uint32_t h);

    void blitImage(VkCommandBuffer cmd, DeviceData& dev,
        VkImage src, VkImage dst,
        uint32_t w, uint32_t h);

    std::mutex mutex_;
    std::unordered_map<void*, DeviceData> devices_;
    std::unordered_map<void*, InstanceData> instances_;

    std::atomic<bool> enabled_{true};
    std::atomic<uint64_t> totalFrames_{0};
    std::atomic<uint64_t> totalInterp_{0};
};

} // namespace framegen

// ─── C entry points ─────────────────────────────────────
extern "C" {
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyInstance(
    VkInstance, const VkAllocationCallbacks*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo*,
    const VkAllocationCallbacks*, VkDevice*);
VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyDevice(
    VkDevice, const VkAllocationCallbacks*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR*,
    const VkAllocationCallbacks*, VkSwapchainKHR*);
VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroySwapchainKHR(
    VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_QueuePresentKHR(
    VkQueue, const VkPresentInfoKHR*);
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetDeviceProcAddr(
    VkDevice, const char*);
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetInstanceProcAddr(
    VkInstance, const char*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceLayerProperties(
    uint32_t*, VkLayerProperties*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceLayerProperties(
    VkPhysicalDevice, uint32_t*, VkLayerProperties*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceExtensionProperties(
    const char*, uint32_t*, VkExtensionProperties*);
VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
} // extern "C"
