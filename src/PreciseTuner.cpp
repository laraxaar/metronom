#include "PreciseTuner.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PreciseTuner::PreciseTuner() {
    m_targetFrequencies = { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f }; // Standard E
}

void PreciseTuner::initialize(uint32_t sampleRate) {
    m_sampleRate = sampleRate;
    m_windowSize = (sampleRate >= 44100) ? 8192 : 4096;
    m_buffer.assign(m_windowSize * 2, 0.0f);
    m_cursor = 0;
    setMode(Mode::GuitarStandard);
}

void PreciseTuner::setMode(Mode mode) {
    m_mode = mode;
    float cutoffHz = 1000.0f;

    switch (mode) {
        case Mode::GuitarStandard:
            m_targetFrequencies = { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f };
            cutoffHz = 1200.0f;
            break;
        case Mode::GuitarDropD:
            m_targetFrequencies = { 73.42f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f };
            cutoffHz = 1200.0f;
            break;
        case Mode::BassStandard:
            m_targetFrequencies = { 41.20f, 55.00f, 73.42f, 98.00f };
            cutoffHz = 400.0f;
            break;
        case Mode::Bass5String:
            m_targetFrequencies = { 30.87f, 41.20f, 55.00f, 73.42f, 98.00f };
            cutoffHz = 400.0f;
            break;
        case Mode::Custom:
            cutoffHz = 1500.0f;
            break;
    }

    // Update LPF for harmonic rejection
    m_lpfAlpha = (2.0f * (float)M_PI * cutoffHz) / (float)m_sampleRate;
    if (m_lpfAlpha > 1.0f) m_lpfAlpha = 1.0f;
}

void PreciseTuner::setCustomTuning(const std::vector<float>& frequencies) {
    m_targetFrequencies = frequencies;
    m_mode = Mode::Custom;
}

void PreciseTuner::process(const float* input, uint32_t nFrames) {
    if (!m_enabled) return;

    for (uint32_t i = 0; i < nFrames; ++i) {
        // Apply LPF (steeper for Bass/Hi-Gain)
        m_lpfState += m_lpfAlpha * (input[i] - m_lpfState);
        m_buffer[m_cursor++] = m_lpfState;

        if (m_cursor >= m_windowSize) {
            float confidence = 0.0f;
            float hz = computeMPM(confidence);

            if (hz > 20.0f && hz < 2000.0f && confidence > 0.4f) {
                m_currentHz.store(hz);
                m_confidence.store(confidence);
            } else {
                m_confidence.store(m_confidence.load() * 0.9f);
            }

            // Overlap (75% overlap)
            size_t shift = m_windowSize / 4;
            std::memmove(m_buffer.data(), m_buffer.data() + shift, (m_windowSize * 2 - shift) * sizeof(float));
            m_cursor -= shift;
        }
    }
}

float PreciseTuner::computeMPM(float& outConfidence) {
    size_t W = m_windowSize / 2;
    std::vector<float> nsdf(W, 0.0f);

    // 1. Normalized Square Difference Function (NSDF) using Autocorrelation
    for (size_t tau = 0; tau < W; tau++) {
        float ac = 0.0f;
        float m = 0.0f;
        for (size_t i = 0; i < W; i++) {
            float x = m_buffer[i];
            float y = m_buffer[i + tau];
            ac += x * y;
            m += x * x + y * y;
        }
        nsdf[tau] = 2.0f * ac / (m + 1e-6f);
    }

    // 2. Peak picking and parabolic interpolation
    size_t bestTau = 0;
    float maxVal = -1.0f;

    // Start after the first zero crossing to avoid the DC/Zero-lag peak
    size_t start = 0;
    while (start < W - 1 && nsdf[start] > 0) start++;
    
    std::vector<size_t> peaks;
    for (size_t tau = std::max((size_t)1, start); tau < W - 1; tau++) {
        if (nsdf[tau] > 0 && nsdf[tau] > nsdf[tau-1] && nsdf[tau] > nsdf[tau+1]) {
            peaks.push_back(tau);
            if (nsdf[tau] > maxVal) maxVal = nsdf[tau];
        }
    }

    if (peaks.empty() || maxVal < 0.3f) {
        outConfidence = 0.0f;
        return 0.0f;
    }

    // Find the first peak that is at least 85% of the absolute maximum (to avoid octave errors)
    for (size_t p : peaks) {
        if (nsdf[p] >= 0.85f * maxVal) {
            bestTau = p;
            break;
        }
    }

    outConfidence = nsdf[bestTau];

    // Parabolic interpolation for sub-sample accuracy
    if (bestTau > 0 && bestTau < W - 1) {
        float s0 = nsdf[bestTau - 1];
        float s1 = nsdf[bestTau];
        float s2 = nsdf[bestTau + 1];
        float den = s0 + s2 - 2 * s1;
        float shift = (std::abs(den) < 1e-6) ? 0 : (s0 - s2) / (2.0f * den);
        
        return static_cast<float>(m_sampleRate) / (static_cast<float>(bestTau) + shift);
    }
    
    return static_cast<float>(m_sampleRate) / static_cast<float>(bestTau);
}

int PreciseTuner::getNearestNoteIndex() const {
    float hz = m_currentHz.load();
    if (hz <= 0) return -1;

    int nearest = -1;
    float minDist = 1e6;
    for (size_t i = 0; i < m_targetFrequencies.size(); ++i) {
        float dist = std::abs(m_targetFrequencies[i] - hz);
        if (dist < minDist) {
            minDist = dist;
            nearest = static_cast<int>(i);
        }
    }
    return nearest;
}

float PreciseTuner::getCentsDeviation() const {
    float hz = m_currentHz.load();
    int idx = getNearestNoteIndex();
    if (hz <= 0 || idx == -1) return 0.0f;

    float target = m_targetFrequencies[idx];
    return 1200.0f * std::log2(hz / target);
}
