#pragma once
#include <atomic>

/**
 * @brief Thread-safe mixer state with 4 channels.
 * All values are 0.0–1.0, atomic for lock-free access.
 */
struct MixerState {
    std::atomic<float> masterVolume{0.8f};
    std::atomic<float> clickVolume{0.7f};    // Normal click level
    std::atomic<float> accentVolume{0.95f};  // Accent click level
    std::atomic<float> inputVolume{0.5f};    // Input (bass) monitoring level

    // Peak meters (written by audio thread, read by UI)
    std::atomic<float> masterPeak{0.0f};
    std::atomic<float> clickPeak{0.0f};
    std::atomic<float> inputPeak{0.0f};

    float applyClick(float sample, float velocity) const {
        float vol = (velocity > 0.9f)
            ? accentVolume.load(std::memory_order_relaxed)
            : clickVolume.load(std::memory_order_relaxed);
        return sample * vol * velocity * masterVolume.load(std::memory_order_relaxed);
    }

    float applyInput(float sample) const {
        return sample * inputVolume.load(std::memory_order_relaxed)
                      * masterVolume.load(std::memory_order_relaxed);
    }

    void reset() {
        masterVolume.store(0.8f); clickVolume.store(0.7f);
        accentVolume.store(0.95f); inputVolume.store(0.5f);
        masterPeak.store(0.0f); clickPeak.store(0.0f); inputPeak.store(0.0f);
    }
};
