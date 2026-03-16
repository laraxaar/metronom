#pragma once
#include <vector>
#include <atomic>
#include <cstdint>

/**
 * @brief High-precision Pitch Detection based on YIN algorithm.
 * Refactored from legacy engine with improved harmonic rejection and stability.
 */
class PreciseTuner {
public:
    PreciseTuner();
    ~PreciseTuner() = default;

    /**
     * @brief Initialize the tuner with sample rate.
     */
    void initialize(uint32_t sampleRate);

    /**
     * @brief Process a block of input samples.
     * @param input Pointer to the input buffer.
     * @param nFrames Number of samples in the buffer.
     */
    void process(const float* input, uint32_t nFrames);

    /**
     * @brief Get the detected frequency in Hz.
     */
    float getFrequency() const { return m_currentHz.load(); }

    /**
     * @brief Get detection confidence (0.0 to 1.0).
     */
    float getConfidence() const { return m_confidence.load(); }

    /**
     * @brief Enable/disable the tuner.
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }

private:
    uint32_t m_sampleRate = 44100;
    bool m_enabled = true;

    // Buffers and indices
    std::vector<float> m_buffer;
    size_t m_cursor = 0;
    size_t m_windowSize = 8192;

    // Results (Thread-safe)
    std::atomic<float> m_currentHz{0.0f};
    std::atomic<float> m_confidence{0.0f};

    // DSP State
    float m_lpfState = 0.0f;
    float m_lpfAlpha = 0.15f;
    float m_prevHz = 0.0f;

    // Constants
    const float m_threshold = 0.10f;
    const float m_hysteresisHz = 1.5f; // Minimum change to update display

    /**
     * @brief Core YIN algorithm implementation.
     */
    float computeYin(float& outConfidence);

    /**
     * @brief Apply a simple LPF to suppress hi-gain harmonics.
     */
    float applyLPF(float sample);
};
