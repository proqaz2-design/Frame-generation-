/**
 * Frame Presenter implementation — the main pipeline orchestrator
 */

#include "frame_presenter.h"
#include <chrono>

namespace framegen {

FramePresenter::~FramePresenter() {
    shutdown();
}

bool FramePresenter::init(const InitParams& params) {
    capture_ = params.capture;
    interpolator_ = params.interpolator;
    device_ = params.device;
    presentQueue_ = params.presentQueue;
    swapchain_ = params.swapchain;
    width_ = params.width;
    height_ = params.height;
    config_ = params.config;

    // Calculate present interval based on target refresh rate
    // 120Hz = 8.33ms, 90Hz = 11.1ms, 60Hz = 16.6ms
    presentIntervalNs_ = 1'000'000'000ULL / config_.target_refresh_rate;

    LOGI("FramePresenter: Initialized %ux%u, target %u Hz (interval %.2f ms)",
         width_, height_, config_.target_refresh_rate, ns_to_ms(presentIntervalNs_));
    return true;
}

void FramePresenter::shutdown() {
    stop();
    capturedQueue_.clear();
    presentQueue_q_.clear();
}

void FramePresenter::start() {
    if (running_) return;

    running_ = true;

    // Start interpolation thread (high priority)
    interpolationThread_ = std::thread([this] {
        // Set thread priority for real-time performance
        // On Android, use nice(-20) or sched_setscheduler
        interpolationLoop();
    });

    // Start presentation thread (highest priority)
    presentationThread_ = std::thread([this] {
        presentationLoop();
    });

    LOGI("FramePresenter: Pipeline started");
}

void FramePresenter::stop() {
    if (!running_) return;

    running_ = false;

    if (interpolationThread_.joinable()) {
        interpolationThread_.join();
    }
    if (presentationThread_.joinable()) {
        presentationThread_.join();
    }

    LOGI("FramePresenter: Pipeline stopped. Generated: %lu, Dropped: %lu",
         stats_.frames_generated.load(), stats_.frames_dropped.load());
}

void FramePresenter::onFrameCaptured(const FrameData& frame) {
    if (!capturedQueue_.push(frame)) {
        stats_.frames_dropped.fetch_add(1);
        LOGW("FramePresenter: Capture queue full, dropping frame %lu", frame.frame_index);
    }
}

// ============================================================
// Interpolation thread — generates in-between frames
// ============================================================
void FramePresenter::interpolationLoop() {
    LOGI("InterpolationThread: Started");

    while (running_) {
        auto frameOpt = capturedQueue_.pop();
        if (!frameOpt) {
            // No frame available — yield briefly
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        FrameData currentFrame = *frameOpt;
        auto captureStart = now_ns();

        if (!hasPreviousFrame_) {
            // First frame — just pass through
            presentQueue_q_.push(currentFrame);
            previousFrame_ = currentFrame;
            hasPreviousFrame_ = true;
            continue;
        }

        // We have two frames — generate interpolated frames
        uint32_t interpCount = getInterpolationCount();

        if (interpCount == 0 || config_.mode == Config::Mode::OFF) {
            // Passthrough mode
            presentQueue_q_.push(currentFrame);
        } else {
            // Push previous frame first
            presentQueue_q_.push(previousFrame_);

            // Generate intermediate frames
            auto interpStart = now_ns();

            std::vector<FrameData> interpolated;
            bool success = interpolator_->interpolateMulti(
                previousFrame_, currentFrame, interpCount, interpolated);

            auto interpEnd = now_ns();
            stats_.interpolation_ms.store(ns_to_ms(interpEnd - interpStart));

            if (success) {
                for (auto& frame : interpolated) {
                    frame.width = width_;
                    frame.height = height_;

                    if (!presentQueue_q_.push(frame)) {
                        stats_.frames_dropped.fetch_add(1);
                        break;
                    }
                    stats_.frames_generated.fetch_add(1);
                }
            } else {
                LOGW("InterpolationThread: Failed to interpolate, passing through");
                stats_.frames_dropped.fetch_add(interpCount);
            }
        }

        auto totalEnd = now_ns();
        stats_.total_ms.store(ns_to_ms(totalEnd - captureStart));

        previousFrame_ = currentFrame;
    }

    LOGI("InterpolationThread: Stopped");
}

// ============================================================
// Presentation thread — delivers frames at exact intervals
// ============================================================
void FramePresenter::presentationLoop() {
    LOGI("PresentationThread: Started, interval=%.2fms", ns_to_ms(presentIntervalNs_));

    uint64_t frameCount = 0;
    auto fpsTimer = now_ns();

    while (running_) {
        uint64_t targetTime = lastPresentNs_ + presentIntervalNs_;
        uint64_t currentTime = now_ns();

        // Precise busy-wait for vsync timing
        // Sleep for most of the remaining time, then spin
        if (currentTime < targetTime) {
            uint64_t remaining = targetTime - currentTime;
            if (remaining > 2'000'000) { // > 2ms → sleep coarsely
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds(remaining - 1'000'000));
            }
            // Spin for the last millisecond
            while (now_ns() < targetTime) {
                // Busy wait — essential for sub-ms precision
            }
        }

        auto frameOpt = presentQueue_q_.pop();
        if (!frameOpt) {
            // No frame ready — missed deadline
            stats_.frames_dropped.fetch_add(1);
            lastPresentNs_ = now_ns();
            continue;
        }

        FrameData frame = *frameOpt;

        auto presentStart = now_ns();
        presentFrame(frame);
        auto presentEnd = now_ns();

        stats_.present_ms.store(ns_to_ms(presentEnd - presentStart));
        lastPresentNs_ = presentEnd;

        frameCount++;

        // Calculate effective FPS every second
        uint64_t elapsed = presentEnd - fpsTimer;
        if (elapsed >= 1'000'000'000ULL) {
            float fps = static_cast<float>(frameCount) * 1'000'000'000.0f / elapsed;
            stats_.effective_fps.store(fps);
            frameCount = 0;
            fpsTimer = presentEnd;

            LOGD("FPS: %.1f | Interp: %.2fms | Present: %.2fms | Queue: %zu",
                 fps, stats_.interpolation_ms.load(),
                 stats_.present_ms.load(), presentQueue_q_.size());
        }
    }

    LOGI("PresentationThread: Stopped");
}

void FramePresenter::presentFrame(const FrameData& frame) {
    if (presentCallback_) {
        presentCallback_(frame);
        return;
    }

    // Default: present via VkQueuePresentKHR
    // In a full implementation, this would:
    // 1. Acquire next swapchain image
    // 2. Copy our interpolated frame to it
    // 3. Present with precise timing

    // For now, signal the Vulkan Present
    if (swapchain_ != VK_NULL_HANDLE && presentQueue_ != VK_NULL_HANDLE) {
        // This is handled by the Vulkan layer in production
    }
}

uint32_t FramePresenter::getInterpolationCount() const {
    switch (config_.mode) {
        case Config::Mode::OFF:
            return 0;
        case Config::Mode::FPS_60:
            return 1;  // 30->60: generate 1 frame between each pair
        case Config::Mode::FPS_90:
            return 2;  // 30->90: generate 2 frames between each pair
        case Config::Mode::FPS_120:
            return 3;  // 30->120: generate 3 frames between each pair
        default:
            return 1;
    }
}

void FramePresenter::setQuality(float quality) {
    config_.quality = quality;
    if (interpolator_) {
        interpolator_->setQuality(quality);
    }
}

} // namespace framegen
