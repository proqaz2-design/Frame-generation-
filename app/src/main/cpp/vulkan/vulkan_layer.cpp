/**
 * FrameGen Vulkan Layer — SELF-CONTAINED implementation.
 *
 * Architecture:
 * - On vkCreateSwapchainKHR: increase minImageCount, store swapchain images
 * - On vkQueuePresentKHR for each game frame N:
 *   1. Copy frame N to staging buffer B
 *   2. If we have previous frame (staging A):
 *      a. Blit previous frame (A) into current swapchain image (replaces game frame)
 *      b. Present (displays the "late" previous frame as an intermediate)
 *      c. Acquire new swapchain image
 *      d. Blit current frame (B) into new image
 *      e. Present (displays actual frame)
 *   3. Swap staging buffers for next iteration
 *
 * Result: 2 presents per game frame = 2× visual framerate.
 * The first present is the previous frame (1 frame latency for the interp slot).
 * Future: replace step 2a with actual optical flow interpolation.
 */

#include "vulkan_layer.h"
#include <cstring>
#include <algorithm>
#include <cinttypes>

namespace framegen {

VulkanLayer& VulkanLayer::instance() {
    static VulkanLayer inst;
    return inst;
}

VulkanLayer::DeviceData& VulkanLayer::getDeviceData(void* key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_[key];
}

VulkanLayer::InstanceData& VulkanLayer::getInstanceData(void* key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return instances_[key];
}

// ================================================================
// Instance creation
// ================================================================
VkResult VulkanLayer::onCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    auto* layerInfo = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);
    while (layerInfo &&
           (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO)) {
        layerInfo = reinterpret_cast<const VkLayerInstanceCreateInfo*>(layerInfo->pNext);
    }
    if (!layerInfo) {
        LOGE("VkLayer: no layer instance link found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto fpGetInstanceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const_cast<VkLayerInstanceCreateInfo*>(layerInfo)->u.pLayerInfo =
        layerInfo->u.pLayerInfo->pNext;

    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    InstanceData data;
    data.instance = *pInstance;
    data.fpGetInstanceProcAddr = fpGetInstanceProcAddr;
    data.fpDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance"));
    data.fpGetPhysMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties"));
    data.fpGetPhysQueueFamilyProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        instances_[getKey(*pInstance)] = data;
    }

    LOGI("=== FrameGen Layer Active === (instance %p)", (void*)*pInstance);
    return VK_SUCCESS;
}

void VulkanLayer::onDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = getKey(instance);
    InstanceData data;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data = instances_[key];
        instances_.erase(key);
    }
    data.fpDestroyInstance(instance, pAllocator);
}

// ================================================================
// Device creation + dispatch table
// ================================================================
VkResult VulkanLayer::onCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    auto* layerInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext);
    while (layerInfo &&
           (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO)) {
        layerInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(layerInfo->pNext);
    }
    if (!layerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    auto fpGetInstanceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto fpGetDeviceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const_cast<VkLayerDeviceCreateInfo*>(layerInfo)->u.pLayerInfo =
        layerInfo->u.pLayerInfo->pNext;

    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));

    VkResult result = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    DeviceData dev;
    dev.device = *pDevice;
    dev.physicalDevice = physicalDevice;
    dev.fpGetDeviceProcAddr = fpGetDeviceProcAddr;

    // Load all needed device functions
    #define LOAD(fn) dev.fp##fn = reinterpret_cast<PFN_vk##fn>( \
        fpGetDeviceProcAddr(*pDevice, "vk" #fn))

    LOAD(DestroyDevice);
    LOAD(QueuePresentKHR);
    LOAD(CreateSwapchainKHR);
    LOAD(DestroySwapchainKHR);
    LOAD(GetSwapchainImagesKHR);
    LOAD(AcquireNextImageKHR);
    LOAD(QueueSubmit);
    LOAD(QueueWaitIdle);
    LOAD(CreateCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(FreeCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdCopyImage);
    LOAD(CmdBlitImage);
    LOAD(CmdPipelineBarrier);
    LOAD(CreateImage);
    LOAD(DestroyImage);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindImageMemory);
    LOAD(GetImageMemoryRequirements);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateSemaphore);
    LOAD(DestroySemaphore);
    LOAD(ResetCommandBuffer);
    LOAD(DeviceWaitIdle);
    #undef LOAD

    // Get first graphics queue family
    dev.graphicsFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    vkGetDeviceQueue(*pDevice, dev.graphicsFamily, 0, &dev.graphicsQueue);

    // Create command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = dev.graphicsFamily;
    dev.fpCreateCommandPool(*pDevice, &poolInfo, nullptr, &dev.cmdPool);

    // Allocate reusable command buffer
    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = dev.cmdPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    dev.fpAllocateCommandBuffers(*pDevice, &cmdInfo, &dev.cmdBuf);

    // Create fence for synchronization
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    dev.fpCreateFence(*pDevice, &fenceInfo, nullptr, &dev.fence);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_[getKey(*pDevice)] = dev;
    }

    LOGI("FrameGen Layer: device created, ready for frame generation");
    return VK_SUCCESS;
}

void VulkanLayer::onDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = getKey(device);
    DeviceData dev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dev = devices_[key];
        devices_.erase(key);
    }

    // Cleanup staging images
    destroyStagingImage(dev, dev.prevFrame);
    destroyStagingImage(dev, dev.curFrame);

    if (dev.fence) dev.fpDestroyFence(device, dev.fence, nullptr);
    if (dev.cmdPool) vkDestroyCommandPool(device, dev.cmdPool, nullptr);

    LOGI("FrameGen Layer: device destroyed (frames: %" PRIu64 ", interp: %" PRIu64 ")",
         dev.frameCount, dev.interpCount);

    dev.fpDestroyDevice(device, pAllocator);
}

// ================================================================
// Swapchain hooks
// ================================================================
VkResult VulkanLayer::onCreateSwapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    void* key = getKey(device);
    DeviceData& dev = getDeviceData(key);

    // Request extra swapchain images for frame doubling
    VkSwapchainCreateInfoKHR modInfo = *pCreateInfo;
    modInfo.minImageCount = std::max(pCreateInfo->minImageCount + 1, 3u);

    // Ensure we can blit to/from swapchain images
    modInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult result = dev.fpCreateSwapchainKHR(device, &modInfo, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) {
        // Fallback: try original params
        result = dev.fpCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        if (result != VK_SUCCESS) return result;
    }

    // Get swapchain images
    SwapchainData scData;
    scData.handle = *pSwapchain;
    scData.format = pCreateInfo->imageFormat;
    scData.width = pCreateInfo->imageExtent.width;
    scData.height = pCreateInfo->imageExtent.height;

    uint32_t imageCount = 0;
    dev.fpGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
    scData.images.resize(imageCount);
    dev.fpGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, scData.images.data());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        dev.swapchains[reinterpret_cast<uint64_t>(*pSwapchain)] = scData;
    }

    // Create staging images for frame capture
    ensureStaging(dev, scData.width, scData.height, scData.format);

    LOGI("FrameGen Layer: swapchain %ux%u, %u images, format %d",
         scData.width, scData.height, imageCount, scData.format);

    return VK_SUCCESS;
}

void VulkanLayer::onDestroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    void* key = getKey(device);
    DeviceData& dev = getDeviceData(key);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        dev.swapchains.erase(reinterpret_cast<uint64_t>(swapchain));
    }

    dev.fpDestroySwapchainKHR(device, swapchain, pAllocator);
}

// ================================================================
// THE KEY FUNCTION: Frame Generation on Present
// ================================================================
VkResult VulkanLayer::onQueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    if (!enabled_ || pPresentInfo->swapchainCount == 0) {
        // Passthrough
        void* key = getKey(queue);
        DeviceData& dev = getDeviceData(key);
        return dev.fpQueuePresentKHR(queue, pPresentInfo);
    }

    void* key = getKey(queue);
    DeviceData& dev = getDeviceData(key);
    dev.frameCount++;
    totalFrames_++;

    // Find swapchain data for the first swapchain
    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t imageIndex = pPresentInfo->pImageIndices[0];

    SwapchainData* scData = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = dev.swapchains.find(reinterpret_cast<uint64_t>(swapchain));
        if (it != dev.swapchains.end()) {
            scData = &it->second;
        }
    }

    if (!scData || scData->images.empty() || imageIndex >= scData->images.size()) {
        return dev.fpQueuePresentKHR(queue, pPresentInfo);
    }

    VkImage gameImage = scData->images[imageIndex];
    uint32_t w = scData->width;
    uint32_t h = scData->height;

    // Ensure staging buffers exist
    ensureStaging(dev, w, h, scData->format);

    if (!dev.curFrame.valid || !dev.prevFrame.valid) {
        // Staging not ready — passthrough
        return dev.fpQueuePresentKHR(queue, pPresentInfo);
    }

    // Wait for previous operations
    dev.fpWaitForFences(dev.device, 1, &dev.fence, VK_TRUE, UINT64_MAX);
    dev.fpResetFences(dev.device, 1, &dev.fence);

    // ─── Step 1: Copy game's frame to curFrame staging ──────
    dev.fpResetCommandBuffer(dev.cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev.fpBeginCommandBuffer(dev.cmdBuf, &beginInfo);

    // Transition game image → TRANSFER_SRC
    transitionImage(dev.cmdBuf, dev, gameImage,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Transition curFrame staging → TRANSFER_DST
    transitionImage(dev.cmdBuf, dev, dev.curFrame.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Copy game image → curFrame staging
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    dev.fpCmdCopyImage(dev.cmdBuf,
        gameImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dev.curFrame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    if (dev.hasPrev) {
        // ─── Step 2: Blit previous frame into current swapchain image ───
        // This effectively inserts the previous frame as an "interpolated" frame
        // before the current frame. For MVP this is frame doubling.
        // Future: replace with optical flow warped blend.

        // Transition prevFrame → TRANSFER_SRC
        transitionImage(dev.cmdBuf, dev, dev.prevFrame.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Transition game image → TRANSFER_DST (to receive the prev frame blit)
        transitionImage(dev.cmdBuf, dev, gameImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Blit prevFrame → gameImage (overwrites with previous frame)
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blitRegion.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blitRegion.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};

        dev.fpCmdBlitImage(dev.cmdBuf,
            dev.prevFrame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            gameImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitRegion, VK_FILTER_NEAREST);

        // Transition game image back → PRESENT_SRC (ready to display)
        transitionImage(dev.cmdBuf, dev, gameImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        // First frame — just transition back for normal present
        transitionImage(dev.cmdBuf, dev, gameImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    dev.fpEndCommandBuffer(dev.cmdBuf);

    // Submit copy/blit commands
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &dev.cmdBuf;

    // Wait on the game's semaphores
    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;
    if (pPresentInfo->waitSemaphoreCount > 0) {
        waitSems.assign(pPresentInfo->pWaitSemaphores,
            pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);
        waitStages.resize(pPresentInfo->waitSemaphoreCount,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
        submitInfo.pWaitSemaphores = waitSems.data();
        submitInfo.pWaitDstStageMask = waitStages.data();
    }

    dev.fpQueueSubmit(queue, 1, &submitInfo, dev.fence);
    dev.fpWaitForFences(dev.device, 1, &dev.fence, VK_TRUE, UINT64_MAX);

    if (dev.hasPrev) {
        // ─── Step 3: Present the intermediate frame ─────────────
        VkPresentInfoKHR interpPresent{};
        interpPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        interpPresent.swapchainCount = 1;
        interpPresent.pSwapchains = &swapchain;
        interpPresent.pImageIndices = &imageIndex;

        VkResult interpResult = dev.fpQueuePresentKHR(queue, &interpPresent);

        if (interpResult == VK_SUCCESS || interpResult == VK_SUBOPTIMAL_KHR) {
            dev.interpCount++;
            totalInterp_++;

            // ─── Step 4: Acquire new image, blit real frame, present ──
            uint32_t newIndex = 0;
            VkResult acqResult = dev.fpAcquireNextImageKHR(
                dev.device, swapchain, UINT64_MAX, VK_NULL_HANDLE, dev.fence, &newIndex);

            if (acqResult == VK_SUCCESS || acqResult == VK_SUBOPTIMAL_KHR) {
                dev.fpWaitForFences(dev.device, 1, &dev.fence, VK_TRUE, UINT64_MAX);
                dev.fpResetFences(dev.device, 1, &dev.fence);

                // Blit current frame from staging to new swapchain image
                dev.fpResetCommandBuffer(dev.cmdBuf, 0);
                dev.fpBeginCommandBuffer(dev.cmdBuf, &beginInfo);

                // curFrame staging → TRANSFER_SRC
                transitionImage(dev.cmdBuf, dev, dev.curFrame.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                // New swapchain image → TRANSFER_DST
                transitionImage(dev.cmdBuf, dev, scData->images[newIndex],
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                // Blit curFrame → new swapchain image
                VkImageBlit blit2{};
                blit2.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit2.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
                blit2.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit2.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};

                dev.fpCmdBlitImage(dev.cmdBuf,
                    dev.curFrame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    scData->images[newIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit2, VK_FILTER_NEAREST);

                // New image → PRESENT_SRC
                transitionImage(dev.cmdBuf, dev, scData->images[newIndex],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

                dev.fpEndCommandBuffer(dev.cmdBuf);

                VkSubmitInfo submit2{};
                submit2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit2.commandBufferCount = 1;
                submit2.pCommandBuffers = &dev.cmdBuf;
                dev.fpQueueSubmit(queue, 1, &submit2, dev.fence);
                dev.fpWaitForFences(dev.device, 1, &dev.fence, VK_TRUE, UINT64_MAX);

                // Present the real frame
                VkPresentInfoKHR realPresent{};
                realPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                realPresent.swapchainCount = 1;
                realPresent.pSwapchains = &swapchain;
                realPresent.pImageIndices = &newIndex;
                dev.fpQueuePresentKHR(queue, &realPresent);
            }
        }
    } else {
        // First frame — just present normally
        VkPresentInfoKHR firstPresent{};
        firstPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        firstPresent.swapchainCount = pPresentInfo->swapchainCount;
        firstPresent.pSwapchains = pPresentInfo->pSwapchains;
        firstPresent.pImageIndices = pPresentInfo->pImageIndices;
        dev.fpQueuePresentKHR(queue, &firstPresent);
    }

    // Swap staging buffers: current becomes previous
    std::swap(dev.prevFrame, dev.curFrame);
    dev.hasPrev = true;

    // Log stats periodically
    if (dev.frameCount % 300 == 0) {
        LOGI("FrameGen: %" PRIu64 " frames, %" PRIu64 " interpolated (%.0f%% boost)",
             dev.frameCount, dev.interpCount,
             dev.frameCount > 0 ? (dev.interpCount * 100.0 / dev.frameCount) : 0.0);
    }

    return VK_SUCCESS;
}

// ================================================================
// Staging image management
// ================================================================
void VulkanLayer::ensureStaging(DeviceData& dev, uint32_t w, uint32_t h, VkFormat fmt) {
    if (dev.curFrame.valid && dev.captureW == w && dev.captureH == h && dev.captureFormat == fmt) {
        return; // Already set up
    }

    // Cleanup old
    dev.fpDeviceWaitIdle(dev.device);
    destroyStagingImage(dev, dev.prevFrame);
    destroyStagingImage(dev, dev.curFrame);

    // Create new
    createStagingImage(dev, dev.prevFrame, w, h, fmt);
    createStagingImage(dev, dev.curFrame, w, h, fmt);

    dev.captureW = w;
    dev.captureH = h;
    dev.captureFormat = fmt;
    dev.hasPrev = false;

    LOGI("FrameGen: staging images created %ux%u", w, h);
}

bool VulkanLayer::createStagingImage(DeviceData& dev, StagingImage& img,
                                     uint32_t w, uint32_t h, VkFormat format) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dev.fpCreateImage(dev.device, &info, nullptr, &img.image) != VK_SUCCESS) {
        LOGE("FrameGen: failed to create staging image");
        return false;
    }

    VkMemoryRequirements memReq;
    dev.fpGetImageMemoryRequirements(dev.device, img.image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(dev, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (dev.fpAllocateMemory(dev.device, &allocInfo, nullptr, &img.memory) != VK_SUCCESS) {
        LOGE("FrameGen: failed to allocate staging memory");
        dev.fpDestroyImage(dev.device, img.image, nullptr);
        img.image = VK_NULL_HANDLE;
        return false;
    }

    dev.fpBindImageMemory(dev.device, img.image, img.memory, 0);
    img.valid = true;
    return true;
}

void VulkanLayer::destroyStagingImage(DeviceData& dev, StagingImage& img) {
    if (img.image != VK_NULL_HANDLE) {
        dev.fpDestroyImage(dev.device, img.image, nullptr);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE) {
        dev.fpFreeMemory(dev.device, img.memory, nullptr);
        img.memory = VK_NULL_HANDLE;
    }
    img.valid = false;
}

uint32_t VulkanLayer::findMemoryType(DeviceData& dev, uint32_t filter,
                                     VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    // Get instance data to call the physical device function
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, inst] : instances_) {
            if (inst.fpGetPhysMemProps) {
                inst.fpGetPhysMemProps(dev.physicalDevice, &memProps);

                for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                    if ((filter & (1 << i)) &&
                        (memProps.memoryTypes[i].propertyFlags & props) == props) {
                        return i;
                    }
                }
                break;
            }
        }
    }
    // Fallback to first matching
    return 0;
}

void VulkanLayer::transitionImage(VkCommandBuffer cmd, DeviceData& dev, VkImage image,
    VkImageLayout oldL, VkImageLayout newL,
    VkAccessFlags srcA, VkAccessFlags dstA,
    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldL;
    barrier.newLayout = newL;
    barrier.srcAccessMask = srcA;
    barrier.dstAccessMask = dstA;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    dev.fpCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ================================================================
// Dispatch table (proc addr routing)
// ================================================================
PFN_vkVoidFunction VulkanLayer::getDeviceProcAddr(VkDevice device, const char* pName) {
    if (!strcmp(pName, "vkQueuePresentKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_QueuePresentKHR);
    if (!strcmp(pName, "vkDestroyDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_DestroyDevice);
    if (!strcmp(pName, "vkCreateSwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_CreateSwapchainKHR);
    if (!strcmp(pName, "vkDestroySwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_DestroySwapchainKHR);
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_GetDeviceProcAddr);

    void* key = getKey(device);
    return devices_[key].fpGetDeviceProcAddr(device, pName);
}

PFN_vkVoidFunction VulkanLayer::getInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!strcmp(pName, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_CreateInstance);
    if (!strcmp(pName, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_DestroyInstance);
    if (!strcmp(pName, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_CreateDevice);
    if (!strcmp(pName, "vkDestroyDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_DestroyDevice);
    if (!strcmp(pName, "vkQueuePresentKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_QueuePresentKHR);
    if (!strcmp(pName, "vkCreateSwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_CreateSwapchainKHR);
    if (!strcmp(pName, "vkDestroySwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_DestroySwapchainKHR);
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_GetDeviceProcAddr);
    if (!strcmp(pName, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_GetInstanceProcAddr);
    if (!strcmp(pName, "vkEnumerateInstanceLayerProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_EnumerateInstanceLayerProperties);
    if (!strcmp(pName, "vkEnumerateDeviceLayerProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_EnumerateDeviceLayerProperties);
    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_EnumerateInstanceExtensionProperties);
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(framegen_EnumerateDeviceExtensionProperties);

    void* key = getKey(instance);
    return instances_[key].fpGetInstanceProcAddr(instance, pName);
}

} // namespace framegen

// ================================================================
// C entry points
// ================================================================
extern "C" {

static const VkLayerProperties layerProps = {
    "VK_LAYER_FRAMEGEN_capture",
    VK_MAKE_VERSION(1, 3, 0),
    1,
    "FrameGen — rootless frame generation layer"
};

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{ return framegen::VulkanLayer::instance().onCreateInstance(pCreateInfo, pAllocator, pInstance); }

VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* pAllocator)
{ framegen::VulkanLayer::instance().onDestroyInstance(instance, pAllocator); }

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{ return framegen::VulkanLayer::instance().onCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice); }

VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroyDevice(
    VkDevice device, const VkAllocationCallbacks* pAllocator)
{ framegen::VulkanLayer::instance().onDestroyDevice(device, pAllocator); }

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{ return framegen::VulkanLayer::instance().onCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain); }

VK_LAYER_EXPORT void VKAPI_CALL framegen_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{ framegen::VulkanLayer::instance().onDestroySwapchain(device, swapchain, pAllocator); }

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{ return framegen::VulkanLayer::instance().onQueuePresent(queue, pPresentInfo); }

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetDeviceProcAddr(
    VkDevice device, const char* pName)
{ return framegen::VulkanLayer::instance().getDeviceProcAddr(device, pName); }

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL framegen_GetInstanceProcAddr(
    VkInstance instance, const char* pName)
{ return framegen::VulkanLayer::instance().getInstanceProcAddr(instance, pName); }

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
    if (!pProperties) { *pPropertyCount = 1; return VK_SUCCESS; }
    if (*pPropertyCount >= 1) {
        memcpy(pProperties, &layerProps, sizeof(VkLayerProperties));
        *pPropertyCount = 1; return VK_SUCCESS;
    }
    return VK_INCOMPLETE;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceLayerProperties(
    VkPhysicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{ return framegen_EnumerateInstanceLayerProperties(pPropertyCount, pProperties); }

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties*)
{
    if (pLayerName && !strcmp(pLayerName, layerProps.layerName)) {
        *pPropertyCount = 0; return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL framegen_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties*)
{
    if (pLayerName && !strcmp(pLayerName, layerProps.layerName)) {
        *pPropertyCount = 0; return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

} // extern "C"
