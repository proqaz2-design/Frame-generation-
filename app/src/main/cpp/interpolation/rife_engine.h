/**
 * RIFE Engine — Real-Time Intermediate Flow Estimation
 *
 * Uses NCNN (Tencent) for neural network inference on mobile GPU.
 * Falls back to GPU compute optical flow when NCNN is not available.
 *
 * The RIFE model predicts bidirectional optical flow between two frames
 * and uses it to synthesize an intermediate frame at any timestep t∈(0,1).
 */

#pragma once

#include "../framegen_types.h"
#include "../vulkan/vulkan_compute.h"

#if NCNN_ENABLED
#include <net.h>
#include <gpu.h>
#endif

#include <memory>
#include <vector>

namespace framegen {

class RifeEngine {
public:
    RifeEngine() = default;
    ~RifeEngine();

    /**
     * Initialize the RIFE model.
     * @param modelDir Path to the NCNN model files (*.param, *.bin)
     * @param compute  VulkanCompute instance for GPU work
     * @param config   Engine configuration
     */
    bool init(const std::string& modelDir, VulkanCompute* compute, const Config& config);
    void shutdown();

    /**
     * Generate an interpolated frame between frame1 and frame2.
     *
     * @param frame1   The earlier frame
     * @param frame2   The later frame
     * @param timestep Interpolation factor (0.0 = frame1, 1.0 = frame2, typically 0.5)
     * @param output   Pre-allocated output FrameData
     * @return true if interpolation succeeded within time budget
     */
    bool interpolate(const FrameData& frame1, const FrameData& frame2,
                     float timestep, FrameData& output);

    /**
     * Generate multiple interpolated frames (for 120Hz from 30fps).
     * @param count Number of intermediate frames to generate
     */
    bool interpolateMulti(const FrameData& frame1, const FrameData& frame2,
                          uint32_t count, std::vector<FrameData>& outputs);

    // Performance
    float getLastInferenceTimeMs() const { return lastInferenceMs_; }
    bool isModelLoaded() const { return modelLoaded_; }

    // Quality control
    void setQuality(float quality); // 0.0-1.0
    void setModelScale(float scale); // Resolution scaling

private:
    VulkanCompute* compute_ = nullptr;
    Config config_;
    bool modelLoaded_ = false;
    float lastInferenceMs_ = 0.0f;

#if NCNN_ENABLED
    // NCNN network for RIFE inference
    ncnn::Net rifeNet_;
    ncnn::VulkanDevice* ncnnVkDevice_ = nullptr;

    bool initNCNN(const std::string& modelDir);
    bool runNCNNInference(const FrameData& frame1, const FrameData& frame2,
                          float timestep, FrameData& output);
#endif

    // Fallback: GPU compute shader optical flow + warping
    bool initFallback();
    bool runFallbackInterpolation(const FrameData& frame1, const FrameData& frame2,
                                   float timestep, FrameData& output);

    // Image scaling helpers
    struct ScaledFrame {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width, height;
    };

    ScaledFrame downscale(const FrameData& frame);
    void upscale(const ScaledFrame& src, FrameData& dst);

    std::vector<ScaledFrame> scaledBuffers_;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
};

} // namespace framegen
