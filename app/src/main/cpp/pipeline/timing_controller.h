/**
 * Timing Controller — manages frame pacing and adaptive quality.
 *
 * Monitors frame times and automatically adjusts quality settings
 * to maintain the target frame rate. Also handles thermal throttling.
 */

#pragma once

#include "../framegen_types.h"
#include <deque>
#include <mutex>

namespace framegen {

class TimingController {
public:
    TimingController() = default;

    void init(Config& config);

    /**
     * Called after each frame is processed. Returns true if we're on budget.
     * If false, the controller has already adjusted quality settings.
     */
    bool onFrameComplete(float frameTimeMs);

    /**
     * Get the current GPU temperature (Celsius).
     * Reads from /sys/class/thermal/ on Android.
     */
    float getGpuTemperature() const;

    /**
     * Returns true if thermal throttling should reduce quality.
     */
    bool isThermalThrottled() const;

    /**
     * Adaptive quality — automatically scale down when over budget.
     */
    struct AdaptiveState {
        float currentScale = 0.5f;  // Current model resolution scale
        float currentQuality = 0.5f;
        float targetMs = 8.0f;       // Target frame time
        float avgMs = 0.0f;
        float maxMs = 0.0f;
        float minMs = 999.0f;
        bool throttled = false;
        int consecutiveOverBudget = 0;
        int consecutiveUnderBudget = 0;
    };

    const AdaptiveState& getState() const { return state_; }

    // Manual overrides
    void setTargetMs(float ms) { state_.targetMs = ms; }
    void setBudget(uint64_t ns) { state_.targetMs = ns_to_ms(ns); }

private:
    Config* config_ = nullptr;
    AdaptiveState state_;
    std::deque<float> frameHistory_;
    static constexpr size_t HISTORY_SIZE = 60;
    mutable std::mutex mutex_;

    float readThermalZone(const char* path) const;
    void adjustQuality(bool overBudget);
};

} // namespace framegen
