/**
 * Timing Controller implementation — frame budget management & thermal protection
 */

#include "timing_controller.h"
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cstdio>
#include <dirent.h>

namespace framegen {

void TimingController::init(Config& config) {
    config_ = &config;
    state_.targetMs = ns_to_ms(config.max_frame_time_ns);
    state_.currentScale = config.model_scale;
    state_.currentQuality = config.quality;

    LOGI("TimingController: Budget=%.2fms, Scale=%.2f, Quality=%.2f",
         state_.targetMs, state_.currentScale, state_.currentQuality);
}

bool TimingController::onFrameComplete(float frameTimeMs) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Track history
    frameHistory_.push_back(frameTimeMs);
    if (frameHistory_.size() > HISTORY_SIZE) {
        frameHistory_.pop_front();
    }

    // Calculate stats
    state_.avgMs = std::accumulate(frameHistory_.begin(), frameHistory_.end(), 0.0f)
                   / frameHistory_.size();
    state_.maxMs = *std::max_element(frameHistory_.begin(), frameHistory_.end());
    state_.minMs = *std::min_element(frameHistory_.begin(), frameHistory_.end());

    bool overBudget = frameTimeMs > state_.targetMs;

    // Consecutive tracking for hysteresis
    if (overBudget) {
        state_.consecutiveOverBudget++;
        state_.consecutiveUnderBudget = 0;
    } else {
        state_.consecutiveUnderBudget++;
        state_.consecutiveOverBudget = 0;
    }

    // Check thermal throttling
    if (config_->thermal_protection) {
        float temp = getGpuTemperature();
        state_.throttled = (temp > 75.0f); // Throttle above 75°C

        if (temp > 85.0f) {
            // Critical — force minimum quality
            state_.currentScale = 0.25f;
            state_.currentQuality = 0.0f;
            config_->model_scale = 0.25f;
            config_->quality = 0.0f;
            LOGW("TimingController: THERMAL CRITICAL (%.1f°C) — minimum quality", temp);
            return false;
        }

        if (state_.throttled && state_.consecutiveOverBudget >= 3) {
            adjustQuality(true);
            return false;
        }
    }

    // Adaptive quality adjustment
    if (state_.consecutiveOverBudget >= 5) {
        adjustQuality(true);
        return false;
    }

    // Scale up if we have headroom
    if (state_.consecutiveUnderBudget >= 30 && state_.avgMs < state_.targetMs * 0.7f) {
        adjustQuality(false);
    }

    return !overBudget;
}

float TimingController::getGpuTemperature() const {
    // Try common thermal zone paths on Android
    static const char* thermalPaths[] = {
        "/sys/class/thermal/thermal_zone0/temp",  // GPU on many SoCs
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/class/thermal/thermal_zone3/temp",  // GPU on Qualcomm
        "/sys/devices/virtual/thermal/thermal_zone0/temp",
    };

    for (const char* path : thermalPaths) {
        float temp = readThermalZone(path);
        if (temp > 0) return temp;
    }

    // Try to find GPU thermal zone dynamically
    DIR* dir = opendir("/sys/class/thermal/");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string typePath = std::string("/sys/class/thermal/") +
                                   entry->d_name + "/type";
            std::ifstream typeFile(typePath);
            if (typeFile.is_open()) {
                std::string type;
                typeFile >> type;
                if (type.find("gpu") != std::string::npos ||
                    type.find("GPU") != std::string::npos) {
                    std::string tempPath = std::string("/sys/class/thermal/") +
                                           entry->d_name + "/temp";
                    float temp = readThermalZone(tempPath.c_str());
                    closedir(dir);
                    if (temp > 0) return temp;
                }
            }
        }
        closedir(dir);
    }

    return 0.0f; // Unknown
}

bool TimingController::isThermalThrottled() const {
    return state_.throttled;
}

float TimingController::readThermalZone(const char* path) const {
    std::ifstream file(path);
    if (!file.is_open()) return -1.0f;

    int rawTemp;
    file >> rawTemp;

    // Android reports in millidegrees Celsius
    if (rawTemp > 1000) {
        return static_cast<float>(rawTemp) / 1000.0f;
    }
    return static_cast<float>(rawTemp);
}

void TimingController::adjustQuality(bool overBudget) {
    if (overBudget) {
        // Reduce quality
        state_.currentScale = std::max(0.25f, state_.currentScale - 0.1f);
        state_.currentQuality = std::max(0.0f, state_.currentQuality - 0.15f);

        LOGI("TimingController: ↓ Scale=%.2f Quality=%.2f (avg=%.2fms, budget=%.2fms)",
             state_.currentScale, state_.currentQuality, state_.avgMs, state_.targetMs);
    } else {
        // Increase quality (slower ramp-up than ramp-down)
        state_.currentScale = std::min(0.75f, state_.currentScale + 0.05f);
        state_.currentQuality = std::min(1.0f, state_.currentQuality + 0.05f);

        LOGI("TimingController: ↑ Scale=%.2f Quality=%.2f (avg=%.2fms, budget=%.2fms)",
             state_.currentScale, state_.currentQuality, state_.avgMs, state_.targetMs);
    }

    // Apply to config
    if (config_) {
        config_->model_scale = state_.currentScale;
        config_->quality = state_.currentQuality;
    }

    // Reset counters
    state_.consecutiveOverBudget = 0;
    state_.consecutiveUnderBudget = 0;
}

} // namespace framegen
