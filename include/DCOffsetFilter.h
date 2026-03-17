#pragma once
#include <cstddef>
#include <cmath>

/**
 * @brief Single-pole high-pass IIR filter for removing DC offset from audio input.
 *
 * Transfer function: y[n] = x[n] - x[n-1] + alpha * y[n-1]
 * 
 * With alpha ≈ 0.995 at 48kHz, the cutoff is approximately 1.6 Hz,
 * which removes any DC bias while preserving the lowest bass frequencies
 * (low B on 5-string bass = 30.87 Hz is well above cutoff).
 *
 * This is header-only for inline use in the audio callback (zero overhead).
 */
class DCOffsetFilter {
public:
    /**
     * @brief Construct with a specific coefficient or default.
     * @param alpha Filter coefficient (0.99–0.999 typical). Higher = lower cutoff.
     */
    explicit DCOffsetFilter(float alpha = 0.995f)
        : m_alpha(alpha)
        , m_prevInput(0.0f)
        , m_prevOutput(0.0f)
    {}

    /**
     * @brief Recalculate alpha from a desired cutoff frequency and sample rate.
     * @param cutoffHz Desired cutoff in Hz (e.g., 1.5 Hz).
     * @param sampleRate Sample rate in Hz (e.g., 48000).
     */
    void setCutoff(float cutoffHz, float sampleRate) {
        // alpha = 1 / (1 + 2*pi*fc/fs)
        // This gives a -3dB point at cutoffHz
        const float w = 2.0f * 3.14159265358979323846f * cutoffHz / sampleRate;
        m_alpha = 1.0f / (1.0f + w);
    }

    /**
     * @brief Process a block of samples in-place.
     * Safe to call from the audio callback (no allocations, no locks).
     * @param buffer Audio samples to filter (modified in-place).
     * @param count Number of samples.
     */
    void process(float* buffer, size_t count) {
        float xPrev = m_prevInput;
        float yPrev = m_prevOutput;

        for (size_t i = 0; i < count; ++i) {
            const float x = buffer[i];
            const float y = x - xPrev + m_alpha * yPrev;
            buffer[i] = y;
            xPrev = x;
            yPrev = y;
        }

        m_prevInput  = xPrev;
        m_prevOutput = yPrev;
    }

    /**
     * @brief Process a single sample (for per-sample pipelines).
     * @param x Input sample.
     * @return Filtered output sample.
     */
    float processSample(float x) {
        const float y = x - m_prevInput + m_alpha * m_prevOutput;
        m_prevInput  = x;
        m_prevOutput = y;
        return y;
    }

    /**
     * @brief Reset filter state (call when switching inputs or on stream restart).
     */
    void reset() {
        m_prevInput  = 0.0f;
        m_prevOutput = 0.0f;
    }

private:
    float m_alpha;
    float m_prevInput;
    float m_prevOutput;
};
