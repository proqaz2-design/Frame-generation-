/**
 * Frame Presenter — delivers frames to the display at precise intervals.
 *
 * Orchestrates the entire pipeline:
 * 1. Takes captured frame pairs from the capture module
 * 2. Feeds them to the interpolation engine
 * 3. Inserts interpolated frames between originals
 * 4. Presents the sequence at the target refresh rate
 *
 * Uses VK_GOOGLE_display_timing (or Choreographer) for precise vsync.
 */

#pragma once

#include "../framegen_types.h"
#include "frame_queue.h"
#include "../interpolation/rife_engine.h"
#include "../vulkan/vulkan_capture.h"

#include <thread>
#include <atomic>
#include <functional>

namespace framegen {

class FramePresenter {
public:
    FramePresenter() = default;
    ~FramePresenter();

    struct InitParams {
        VulkanCapture* capture = nullptr;
        RifeEngine* interpolator = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue presentQueue = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        Config config;
    };

    bool init(const InitParams& params);
    void shutdown();

    // Start/stop the presentation loop
    void start();
    void stop();

    // Feed a new captured frame into the pipeline
    void onFrameCaptured(const FrameData& frame);

    // Performance stats
    PerfStats& getStats() { return stats_; }
    const PerfStats& getStats() const { return stats_; }

    // Runtime controls
    void setMode(Config::Mode mode) { config_.mode = mode; }
    void setQuality(float quality);

    // Callback when a frame is ready for display
    using PresentCallback = std::function<void(const FrameData& frame)>;
    void setPresentCallback(PresentCallback cb) { presentCallback_ = std::move(cb); }

private:
    Config config_;
    VulkanCapture* capture_ = nullptr;
    RifeEngine* interpolator_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;

    // Frame queues
    FrameQueue<8> capturedQueue_;  // Raw captured frames
    FrameQueue<16> presentQueue_q_; // Frames ready to present (including interpolated)

    // Threading
    std::thread interpolationThread_;
    std::thread presentationThread_;
    std::atomic<bool> running_{false};

    PerfStats stats_;
    PresentCallback presentCallback_;

    // Previous frame for interpolation pairs
    FrameData previousFrame_;
    bool hasPreviousFrame_ = false;

    // Worker threads
    void interpolationLoop();
    void presentationLoop();

    // Timing
    uint64_t presentIntervalNs_ = 0; // Time between presented frames
    uint64_t lastPresentNs_ = 0;

    // Determine how many interpolated frames to generate
    uint32_t getInterpolationCount() const;

    // Present a single frame with vsync timing
    void presentFrame(const FrameData& frame);
};

} // namespace framegen
