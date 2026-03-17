#pragma once
#include <atomic>
#include <cstdint>

/**
 * @brief Thread-safe structure for passing pitch detection results
 * from the analysis thread back to the GUI/logic thread.
 *
 * All fields are atomic for lock-free reads from any thread.
 * The audio callback writes to this; the GUI reads from it.
 */
struct PitchResult {
    /** Detected fundamental frequency in Hz (0 if no pitch detected). */
    std::atomic<float> frequency{0.0f};

    /** Detection confidence [0.0 .. 1.0]. Below 0.5 is unreliable. */
    std::atomic<float> confidence{0.0f};

    /** Deviation in cents from the nearest target note (-50..+50). */
    std::atomic<float> cents{0.0f};

    /** Index of nearest note in the current tuning preset (-1 if none). */
    std::atomic<int> noteIndex{-1};

    /** Sample position (absolute frame count) when this result was computed. */
    std::atomic<uint64_t> timestamp{0};

    /** RMS level of the analysis window (for signal presence detection). */
    std::atomic<float> rmsLevel{0.0f};

    /** Whether pitch tracking is currently active (signal above noise floor). */
    std::atomic<bool> active{false};

    /** Reset all fields to defaults. Not thread-safe — call only when stream is stopped. */
    void reset() {
        frequency.store(0.0f, std::memory_order_relaxed);
        confidence.store(0.0f, std::memory_order_relaxed);
        cents.store(0.0f, std::memory_order_relaxed);
        noteIndex.store(-1, std::memory_order_relaxed);
        timestamp.store(0, std::memory_order_relaxed);
        rmsLevel.store(0.0f, std::memory_order_relaxed);
        active.store(false, std::memory_order_relaxed);
    }

    // Non-copyable (atomics)
    PitchResult() = default;
    PitchResult(const PitchResult&) = delete;
    PitchResult& operator=(const PitchResult&) = delete;
};
