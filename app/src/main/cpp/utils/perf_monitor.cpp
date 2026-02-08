/**
 * Performance Monitor implementation
 */

#include "perf_monitor.h"
#include <sstream>
#include <iomanip>

namespace framegen {

void PerfMonitor::init() {
    stats_.capture_ms.store(0.0f);
    stats_.motion_est_ms.store(0.0f);
    stats_.interpolation_ms.store(0.0f);
    stats_.present_ms.store(0.0f);
    stats_.total_ms.store(0.0f);
    stats_.frames_generated.store(0);
    stats_.frames_dropped.store(0);
    stats_.gpu_temp_celsius.store(0.0f);
    stats_.effective_fps.store(0.0f);
    lastReportNs_ = now_ns();
}

void PerfMonitor::beginCapture()          { captureStart_ = now_ns(); }
void PerfMonitor::beginMotionEstimation() { motionStart_  = now_ns(); }
void PerfMonitor::beginInterpolation()    { interpStart_  = now_ns(); }
void PerfMonitor::beginPresent()          { presentStart_ = now_ns(); }

void PerfMonitor::endCapture() {
    stats_.capture_ms.store(ns_to_ms(now_ns() - captureStart_));
}

void PerfMonitor::endMotionEstimation() {
    stats_.motion_est_ms.store(ns_to_ms(now_ns() - motionStart_));
}

void PerfMonitor::endInterpolation() {
    stats_.interpolation_ms.store(ns_to_ms(now_ns() - interpStart_));
}

void PerfMonitor::endPresent() {
    uint64_t endNs = now_ns();
    stats_.present_ms.store(ns_to_ms(endNs - presentStart_));
    stats_.total_ms.store(
        stats_.capture_ms.load() +
        stats_.motion_est_ms.load() +
        stats_.interpolation_ms.load() +
        stats_.present_ms.load()
    );

    // Report to Java side periodically
    if (statsCallback_ && (endNs - lastReportNs_) >= REPORT_INTERVAL_NS) {
        statsCallback_(stats_);
        lastReportNs_ = endNs;
    }
}

std::string PerfMonitor::getOverlayText() const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    ss << "FPS: " << stats_.effective_fps.load() << "\n";
    ss << "Cap: " << stats_.capture_ms.load() << "ms\n";
    ss << "MV:  " << stats_.motion_est_ms.load() << "ms\n";
    ss << "AI:  " << stats_.interpolation_ms.load() << "ms\n";
    ss << "Pre: " << stats_.present_ms.load() << "ms\n";
    ss << "Tot: " << stats_.total_ms.load() << "ms\n";
    ss << "Gen: " << stats_.frames_generated.load()
       << " Drop: " << stats_.frames_dropped.load() << "\n";
    ss << "GPU: " << stats_.gpu_temp_celsius.load() << "°C";
    return ss.str();
}

} // namespace framegen
