/**
 * Optical Flow — GLSL compute shader logic compiled to SPIR-V.
 *
 * This file contains documentation for the compute shader algorithms
 * and the C++ host-side integration code.
 */

#pragma once

#include "../framegen_types.h"
#include "../vulkan/vulkan_compute.h"

namespace framegen {

/**
 * Manages the optical flow compute pipeline.
 * The actual shader code is in shaders/*.comp files compiled to SPIR-V.
 *
 * Pipeline stages:
 * 1. Grayscale conversion (RGB -> Luma)
 * 2. Gaussian blur pyramid
 * 3. Block matching with SAD (Sum of Absolute Differences)
 * 4. Sub-pixel refinement
 * 5. Median filter for outlier removal
 * 6. Forward-backward consistency check
 */
class OpticalFlow {
public:
    OpticalFlow() = default;
    ~OpticalFlow();

    bool init(VulkanCompute* compute, uint32_t width, uint32_t height);
    void shutdown();

    struct FlowResult {
        VkImage forwardFlow;     // Frame1 -> Frame2 motion
        VkImage backwardFlow;    // Frame2 -> Frame1 motion
        VkImage confidenceMap;   // Per-pixel confidence
        VkImageView forwardFlowView;
        VkImageView backwardFlowView;
        VkImageView confidenceView;
        float executionTimeMs;
    };

    /**
     * Compute bidirectional optical flow.
     * Forward flow: where each pixel in frame1 moves to in frame2
     * Backward flow: where each pixel in frame2 moves to in frame1
     * Confidence: consistency check between forward/backward
     */
    FlowResult computeBidirectional(const FrameData& frame1, const FrameData& frame2,
                                     VkSemaphore waitSem = VK_NULL_HANDLE);

private:
    VulkanCompute* compute_ = nullptr;
    uint32_t width_ = 0, height_ = 0;

    // GPU resources for intermediate results
    struct FlowImages {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    FlowImages forwardFlow_;
    FlowImages backwardFlow_;
    FlowImages confidenceMap_;
    FlowImages grayscale1_;
    FlowImages grayscale2_;

    bool createFlowImage(FlowImages& img, VkFormat format, uint32_t width, uint32_t height);
    void destroyFlowImage(FlowImages& img);
};

} // namespace framegen
