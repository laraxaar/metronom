#include "SmartOnsetDetector.h"
#include <cmath>

void SmartOnsetDetector::reset() {
    m_energyHistory.clear();
    m_prevEnergy = 0.0f;
    m_adaptiveThr = baseThreshold;
    m_lastHitSec = 0.0;
    m_rapidHitCount = 0;
    m_prevFlux = 0.0;
}

bool SmartOnsetDetector::processWindow(const float* samples, int n, uint32_t sr, double currentBpm, double currentSec) {
    // 1. RMS energy
    float sum2 = 0.0f;
    for (int i = 0; i < n; ++i) sum2 += samples[i] * samples[i];
    float rms = std::sqrt(sum2 / std::max(1, n));

    // 2. Spectral flux (energy derivative)
    float flux = rms - m_prevEnergy;
    m_prevEnergy = rms;

    m_energyHistory.push_back(rms);
    if (m_energyHistory.size() > 200) m_energyHistory.pop_front();

    // 3. Adaptive threshold
    if (m_energyHistory.size() > 30) {
        std::vector<float> tmp(m_energyHistory.begin(), m_energyHistory.end());
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        m_adaptiveThr = tmp[tmp.size() / 2] * 4.0 + baseThreshold;
    }

    // 4. Must exceed adaptive threshold
    if (rms <= m_adaptiveThr) {
        if (rms < m_adaptiveThr * 0.5) m_rapidHitCount = 0;
        return false;
    }

    // 5. Spectral flux gate
    if (useSpectralFlux && flux < fluxThreshold) {
        m_prevFlux = flux;
        return false;
    }
    m_prevFlux = flux;

    // 6. BPM-context-aware debounce
    double beatInterval = (currentBpm > 0) ? (60.0 / currentBpm) : 0.5;
    double debounce = std::max(debounceMinSec, beatInterval * bpmDebounceFactor);

    double gap = currentSec - m_lastHitSec;
    if (gap < debounce) {
        m_rapidHitCount++;
        return false;
    }

    // 7. Tremolo burst suppression
    if (m_rapidHitCount > 5) {
        if (flux < fluxThreshold * 4.0) return false;
    }

    // 8. Adaptive Flux Threshold
    if (m_adaptiveThr > 0.3) {
        if (flux < fluxThreshold * 2.0) return false;
    }

    m_lastHitSec = currentSec;
    m_rapidHitCount = 0;
    return true;
}
