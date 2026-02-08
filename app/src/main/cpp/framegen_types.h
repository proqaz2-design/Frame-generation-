/**
 * FrameGen — Android Frame Generation Engine
 * 
 * Core types and configuration shared across all modules.
 * All time values in nanoseconds unless stated otherwise.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <vulkan/vulkan.h>
#include <android/log.h>

// ============================================================
// Logging
// ============================================================
#define FG_TAG "FrameGen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  FG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  FG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, FG_TAG, __VA_ARGS__)

// ============================================================
// Configuration
// ============================================================
namespace framegen {

struct Config {
    // Target interpolation mode
    enum class Mode : uint8_t {
        OFF       = 0,  // Passthrough
        FPS_60    = 1,  // 30->60 (1 interpolated frame)
        FPS_90    = 2,  // 30->90 (2 interpolated frames)
        FPS_120   = 3,  // 30->120 (3 interpolated) or 60->120 (1 interpolated)
    };

    Mode mode = Mode::FPS_60;

    // Maximum time budget for one interpolated frame (nanoseconds)
    uint64_t max_frame_time_ns = 8'000'000;  // 8ms

    // Quality vs speed trade-off (0.0 = fastest, 1.0 = best quality)
    float quality = 0.5f;

    // Resolution scale for the AI model (1.0 = full res, 0.5 = half res)
    float model_scale = 0.5f;

    // Number of frames in the ring buffer
    uint32_t ring_buffer_size = 4;

    // Enable GPU thermal throttling protection
    bool thermal_protection = true;

    // Target screen refresh rate (Hz)
    uint32_t target_refresh_rate = 120;
};

// ============================================================
// Frame descriptor
// ============================================================
struct FrameData {
    VkImage         image         = VK_NULL_HANDLE;
    VkImageView     image_view    = VK_NULL_HANDLE;
    VkDeviceMemory  memory        = VK_NULL_HANDLE;
    VkFramebuffer   framebuffer   = VK_NULL_HANDLE;

    uint32_t width  = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    uint64_t timestamp_ns  = 0;  // When this frame was captured
    uint64_t frame_index   = 0;  // Monotonic frame counter
    bool     is_interpolated = false;

    // For GPU sync
    VkSemaphore render_complete = VK_NULL_HANDLE;
    VkFence     fence           = VK_NULL_HANDLE;
};

// ============================================================
// Motion vector for a block
// ============================================================
struct MotionVector {
    float dx;   // Horizontal displacement (pixels)
    float dy;   // Vertical displacement (pixels)
    float confidence; // 0.0–1.0, how reliable this vector is
};

// ============================================================
// Performance stats
// ============================================================
struct PerfStats {
    std::atomic<float>    capture_ms{0.0f};
    std::atomic<float>    motion_est_ms{0.0f};
    std::atomic<float>    interpolation_ms{0.0f};
    std::atomic<float>    present_ms{0.0f};
    std::atomic<float>    total_ms{0.0f};
    std::atomic<uint64_t> frames_generated{0};
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<float>    gpu_temp_celsius{0.0f};
    std::atomic<float>    effective_fps{0.0f};
};

// Clock helper
using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::steady_clock::time_point;

inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

inline float ns_to_ms(uint64_t ns) {
    return static_cast<float>(ns) / 1'000'000.0f;
}

} // namespace framegen
