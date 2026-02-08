/**
 * Performance Monitor — real-time stats and overlay
 */

#pragma once

#include "../framegen_types.h"
#include <string>
#include <functional>

namespace framegen {

class PerfMonitor {
public:
    PerfMonitor() = default;

    void init();

    // Call at the start/end of each pipeline stage
    void beginCapture();
    void endCapture();
    void beginMotionEstimation();
    void endMotionEstimation();
    void beginInterpolation();
    void endInterpolation();
    void beginPresent();
    void endPresent();

    // Get current stats
    const PerfStats& getStats() const { return stats_; }

    // Get formatted stats string for overlay
    std::string getOverlayText() const;

    // Callback to send stats to Java side
    using StatsCallback = std::function<void(const PerfStats&)>;
    void setStatsCallback(StatsCallback cb) { statsCallback_ = std::move(cb); }

private:
    PerfStats stats_;
    StatsCallback statsCallback_;

    uint64_t captureStart_ = 0;
    uint64_t motionStart_ = 0;
    uint64_t interpStart_ = 0;
    uint64_t presentStart_ = 0;

    uint64_t lastReportNs_ = 0;
    static constexpr uint64_t REPORT_INTERVAL_NS = 500'000'000; // 500ms
};

} // namespace framegen
