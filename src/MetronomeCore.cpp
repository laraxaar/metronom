#include "MetronomeCore.h"
#include <algorithm>
#include <cmath>

void MetronomeCore::addTempoPoint(double timeStart, double timeEnd, double bpmStart, double bpmEnd) {
    m_tempoMap.push_back({timeStart, timeEnd, bpmStart, bpmEnd});
    // Ensure map is sorted by start time
    std::sort(m_tempoMap.begin(), m_tempoMap.end(), [](const TempoPoint& a, const TempoPoint& b) {
        return a.timeStart < b.timeStart;
    });
}

double MetronomeCore::getBpmAtTime(double timeSec) const {
    if (m_tempoMap.empty()) {
        return m_targetBpm.load(std::memory_order_relaxed);
    }

    // Find the current active tempo point or ramp
    for (const auto& point : m_tempoMap) {
        if (timeSec >= point.timeStart && timeSec < point.timeEnd) {
            if (point.bpmStart == point.bpmEnd) {
                return point.bpmStart;
            } else {
                // Linear interpolation (ramp)
                double progress = (timeSec - point.timeStart) / (point.timeEnd - point.timeStart);
                return point.bpmStart + (point.bpmEnd - point.bpmStart) * progress;
            }
        }
    }
    
    // Fallback: If before the first point, use target BPM
    if (timeSec < m_tempoMap.front().timeStart) {
        return m_targetBpm.load(std::memory_order_relaxed);
    }

    // Fallback: If after the last point, use the end BPM of the last point
    if (timeSec >= m_tempoMap.back().timeEnd) {
        return m_tempoMap.back().bpmEnd;
    }

    return m_targetBpm.load(std::memory_order_relaxed);
}

void MetronomeCore::reset() {
    m_currentPhase = 0.0;
    m_totalElapsedSec = 0.0;
    m_currentBeatIndex = 0;
    m_nextBeatThreshold = 1.0;
}

void MetronomeCore::process(uint32_t nFrames, uint32_t sampleRate, std::vector<uint32_t>& outBeatOffsets, std::vector<int>& outBeatIndices) {
    outBeatOffsets.clear();
    outBeatIndices.clear();
    
    double sampleDuration = 1.0 / static_cast<double>(sampleRate);
    int subdivision = m_subdivision.load(std::memory_order_relaxed);
    int top = m_timeSigTop.load(std::memory_order_relaxed);
    float groove = m_grooveIntensity.load(std::memory_order_relaxed);
    if (top < 1) top = 1;

    for (uint32_t i = 0; i < nFrames; ++i) {
        double currentBpm = getBpmAtTime(m_totalElapsedSec);
        
        // Duration of one "subdivided" beat in seconds
        double beatDuration = (60.0 / currentBpm) / static_cast<double>(subdivision);
        
        // Advance phase
        m_currentPhase += sampleDuration / beatDuration;
        
        if (m_currentPhase >= m_nextBeatThreshold) {
            outBeatOffsets.push_back(i);
            outBeatIndices.push_back(m_currentBeatIndex);
            
            m_currentBeatIndex = (m_currentBeatIndex + 1) % top;
            
            // Apply groove (micro-timing variations)
            // m_nextBeatThreshold varies around 1.0 based on groove intensity
            if (groove > 0.01f) {
                // Pseudo-random jitter for "unstable" feel
                // In a real scenario, use a more deterministic but organic-feeling function or noise
                static float seed = 0.0f;
                seed += 0.137f;
                float jitter = (std::sin(seed) * 0.5f + 0.5f) * groove * 0.05f; // +/- 2.5% max jitter
                m_nextBeatThreshold = 1.0 + (jitter - (groove * 0.025f)); 
            } else {
                m_nextBeatThreshold = 1.0;
            }
            
            m_currentPhase -= 1.0; // Subtract ideal phase, not actual threshold to maintain long-term stability
            if (m_currentPhase < 0.0) m_currentPhase = 0.0;
            if (m_currentPhase > 1.0) m_currentPhase = 0.0;
        }
        
        m_totalElapsedSec += sampleDuration;
    }
}
