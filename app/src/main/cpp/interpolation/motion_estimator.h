/**
 * Motion Estimator — GPU-accelerated optical flow for motion vectors.
 *
 * Implements a hierarchical block-matching algorithm that runs
 * entirely on the GPU via Vulkan compute shaders.
 *
 * Algorithm:
 * 1. Build image pyramid (4 levels)
 * 2. Coarse-to-fine block matching at each level
 * 3. Refine motion vectors with sub-pixel accuracy
 * 4. Output dense motion field for the interpolator
 */

#pragma once

#include "../framegen_types.h"
#include "../vulkan/vulkan_compute.h"
#include <vector>

namespace framegen {

class MotionEstimator {
public:
    MotionEstimator() = default;
    ~MotionEstimator();

    bool init(VulkanCompute* compute, uint32_t width, uint32_t height);
    void shutdown();

    /**
     * Estimate motion vectors between two frames.
     * The output is a VkImage containing 2D motion vectors (RG16F format).
     *
     * @param frame1   Source frame
     * @param frame2   Target frame
     * @param flowOut  Output image with motion vectors (dx, dy per pixel)
     * @return Execution time in milliseconds
     */
    float estimate(const FrameData& frame1, const FrameData& frame2,
                   VkImage flowOut, VkSemaphore waitSem = VK_NULL_HANDLE);

    // Get the motion vector image view for binding
    VkImageView getFlowImageView() const { return flowImageView_; }
    VkImage getFlowImage() const { return flowImage_; }

    // Configuration
    void setBlockSize(uint32_t size) { blockSize_ = size; }
    void setSearchRadius(uint32_t radius) { searchRadius_ = radius; }
    void setPyramidLevels(uint32_t levels) { pyramidLevels_ = levels; }

private:
    VulkanCompute* compute_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    uint32_t blockSize_ = 8;      // Block matching block size
    uint32_t searchRadius_ = 16;  // Search area radius in pixels
    uint32_t pyramidLevels_ = 4;  // Image pyramid depth

    // Flow field (RG16F = 2x float16 per pixel)
    VkImage flowImage_ = VK_NULL_HANDLE;
    VkImageView flowImageView_ = VK_NULL_HANDLE;
    VkDeviceMemory flowMemory_ = VK_NULL_HANDLE;

    // Image pyramid for multi-scale estimation
    struct PyramidLevel {
        VkImage image1 = VK_NULL_HANDLE;
        VkImage image2 = VK_NULL_HANDLE;
        VkImageView view1 = VK_NULL_HANDLE;
        VkImageView view2 = VK_NULL_HANDLE;
        VkDeviceMemory mem1 = VK_NULL_HANDLE;
        VkDeviceMemory mem2 = VK_NULL_HANDLE;
        VkImage flow = VK_NULL_HANDLE;
        VkImageView flowView = VK_NULL_HANDLE;
        VkDeviceMemory flowMem = VK_NULL_HANDLE;
        uint32_t width, height;
    };

    std::vector<PyramidLevel> pyramid_;

    bool createFlowField();
    bool createPyramid();
    void destroyPyramid();
};

} // namespace framegen
