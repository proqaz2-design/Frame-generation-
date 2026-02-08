/**
 * Frame Queue — Lock-free ring buffer for frame delivery.
 *
 * Manages the flow: Captured → Interpolated → Presented
 * Uses triple buffering to avoid stalls.
 */

#pragma once

#include "../framegen_types.h"
#include <array>
#include <atomic>
#include <optional>

namespace framegen {

/**
 * Single-producer, single-consumer lock-free frame queue.
 * Producer: capture/interpolation thread
 * Consumer: presenter thread
 */
template<size_t Capacity = 8>
class FrameQueue {
public:
    FrameQueue() = default;

    bool push(const FrameData& frame) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % Capacity;

        if (next == tail_.load(std::memory_order_acquire)) {
            // Queue full — drop frame
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buffer_[head] = frame;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<FrameData> pop() {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Empty
        }

        FrameData frame = buffer_[tail];
        tail_.store((tail + 1) % Capacity, std::memory_order_release);
        return frame;
    }

    std::optional<FrameData> peek() const {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return buffer_[tail];
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity - tail + head);
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() == Capacity - 1; }

    uint64_t droppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }
    void resetStats() { droppedFrames_.store(0, std::memory_order_relaxed); }

    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    std::array<FrameData, Capacity> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::atomic<uint64_t> droppedFrames_{0};
};

} // namespace framegen
