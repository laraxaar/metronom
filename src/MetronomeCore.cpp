#include "MetronomeCore.h"
#include <algorithm>
#include <cmath>

void MetronomeCore::reset() {
    m_currentPhase = 0.0;
    m_totalElapsedSec = 0.0;
}

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

void MetronomeCore::process(uint32_t nFrames, uint32_t sampleRate, std::vector<uint32_t>& outBeatOffsets) {
    outBeatOffsets.clear();
    
    double sampleDuration = 1.0 / static_cast<double>(sampleRate);
    int subdivision = m_subdivision.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < nFrames; ++i) {
        double currentBpm = getBpmAtTime(m_totalElapsedSec);
        
        // Duration of one "subdivided" beat in seconds
        // Quarter note = 60/BPM. 
        // Subdivision 1 = Quarter, 2 = Eighth, 4 = Sixteenth.
        double beatDuration = (60.0 / currentBpm) / static_cast<double>(subdivision);
        
        // Advance phase
        m_currentPhase += sampleDuration / beatDuration;
        
        if (m_currentPhase >= 1.0) {
            outBeatOffsets.push_back(i);
            m_currentPhase -= 1.0;
            
            // Prevent phase drift/accumulation for very high subdivisions
            if (m_currentPhase > 1.0) m_currentPhase = 0.0;
        }
        
        m_totalElapsedSec += sampleDuration;
    }
}
