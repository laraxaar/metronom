#include "PreciseTuner.h"
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PreciseTuner::PreciseTuner() {
    m_windowSize = 8192;
    m_buffer.assign(m_windowSize * 2, 0.0f);
}

void PreciseTuner::initialize(uint32_t sampleRate) {
    m_sampleRate = sampleRate;
    m_windowSize = (sampleRate >= 44100) ? 8192 : 4096;
    m_buffer.assign(m_windowSize * 2, 0.0f);
    m_cursor = 0;
    m_lpfState = 0.0f;
    // alpha = 2*pi*fc / sr, where fc = 1000Hz (harmonic suppression)
    m_lpfAlpha = (2.0f * (float)M_PI * 1000.0f) / (float)sampleRate;
    if (m_lpfAlpha > 1.0f) m_lpfAlpha = 1.0f;
}

void PreciseTuner::process(const float* input, uint32_t nFrames) {
    if (!m_enabled) return;

    for (uint32_t i = 0; i < nFrames; ++i) {
        float sample = applyLPF(input[i]);
        m_buffer[m_cursor++] = sample;

        if (m_cursor >= m_windowSize) {
            float confidence = 0.0f;
            float hz = computeYin(confidence);

            if (hz > 20.0f && hz < 2000.0f && confidence > 0.4f) {
                // Hysteresis: Only update frequency if change is significant
                if (std::abs(hz - m_prevHz) > m_hysteresisHz || m_prevHz == 0.0f) {
                    m_currentHz.store(hz);
                    m_prevHz = hz;
                }
                m_confidence.store(confidence);
            } else {
                // Graceful decay of confidence
                m_confidence.store(m_confidence.load() * 0.9f);
            }

            // 75% overlap for faster updates
            size_t shift = m_windowSize / 4;
            std::memmove(m_buffer.data(), m_buffer.data() + shift, (m_windowSize - shift) * sizeof(float));
            m_cursor -= shift;
        }
    }
}

float PreciseTuner::applyLPF(float sample) {
    m_lpfState += m_lpfAlpha * (sample - m_lpfState);
    return m_lpfState;
}

float PreciseTuner::computeYin(float& outConfidence) {
    size_t W = m_windowSize / 2;
    std::vector<float> yin(W, 0.0f);

    // 1. Difference function
    for (size_t tau = 1; tau < W; ++tau) {
        for (size_t i = 0; i < W; ++i) {
            float delta = m_buffer[i] - m_buffer[i + tau];
            yin[tau] += delta * delta;
        }
    }

    // 2. Cumulative mean normalized difference
    float runningSum = 0.0f;
    yin[0] = 1.0f;
    for (size_t tau = 1; tau < W; ++tau) {
        runningSum += yin[tau];
        yin[tau] = yin[tau] * tau / (runningSum + 1e-6f);
    }

    // 3. Absolute threshold — find first dip below threshold
    size_t estimateTau = 0;
    float bestVal = 1.0f;
    for (size_t tau = 2; tau < W; ++tau) {
        if (yin[tau] < m_threshold) {
            while (tau + 1 < W && yin[tau + 1] < yin[tau]) {
                tau++;
            }
            estimateTau = tau;
            bestVal = yin[tau];
            break;
        }
    }

    outConfidence = (estimateTau > 0) ? std::max(0.0f, 1.0f - bestVal) : 0.0f;

    // 4. Parabolic interpolation
    if (estimateTau > 0 && estimateTau < W - 1) {
        float s0 = yin[estimateTau - 1];
        float s1 = yin[estimateTau];
        float s2 = yin[estimateTau + 1];
        float denom = 2.0f * s1 - s2 - s0;
        if (std::abs(denom) > 1e-6f) {
            float shift = 0.5f * (s2 - s0) / denom;
            return (float)m_sampleRate / (estimateTau + shift);
        }
        return (float)m_sampleRate / static_cast<float>(estimateTau);
    }

    return 0.0f;
}
