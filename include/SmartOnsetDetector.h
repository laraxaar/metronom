#pragma once
#include <deque>
#include <cstdint>
#include <vector>
#include <algorithm>

/**
 * @brief Tremolo-safe onset detector using spectral flux and adaptive thresholding.
 * Refactored from legacy engi.cpp.
 */
class SmartOnsetDetector {
public:
    SmartOnsetDetector() = default;

    void reset();

    /**
     * @brief Process a window of samples to detect an onset.
     * @param samples Current window of samples.
     * @param n Number of samples in the window.
     * @param sr Sample rate.
     * @param currentBpm Current BPM (for context-aware debouncing).
     * @param currentSec Current time in seconds.
     * @return true if a genuine onset is detected.
     */
    bool processWindow(const float* samples, int n, uint32_t sr, double currentBpm, double currentSec);

    // Parameters
    double baseThreshold = 0.08;
    double debounceMinSec = 0.04;
    double bpmDebounceFactor = 0.35;
    bool useSpectralFlux = true;
    double fluxThreshold = 0.02;

private:
    std::deque<float> m_energyHistory;
    float m_prevEnergy = 0.0f;
    double m_adaptiveThr = 0.08;
    double m_lastHitSec = 0.0;
    int m_rapidHitCount = 0;
    double m_prevFlux = 0.0;
};
