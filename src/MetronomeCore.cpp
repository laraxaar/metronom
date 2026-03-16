#include "MetronomeCore.h"
#include <algorithm>
#include <cmath>

void MetronomeCore::reset() {
    m_currentPhase = 0.0;
    m_totalElapsedSec = 0.0;
}

void MetronomeCore::addTempoPoint(double timeSec, double bpm) {
    m_tempoMap.push_back({timeSec, bpm});
    // Ensure map is sorted by time
    std::sort(m_tempoMap.begin(), m_tempoMap.end(), [](const TempoPoint& a, const TempoPoint& b) {
        return a.timeSec < b.timeSec;
    });
}

double MetronomeCore::getBpmAtTime(double timeSec) const {
    if (m_tempoMap.empty()) {
        return m_targetBpm.load(std::memory_order_relaxed);
    }

    if (timeSec <= m_tempoMap.front().timeSec) {
        return m_tempoMap.front().bpm;
    }

    if (timeSec >= m_tempoMap.back().timeSec) {
        return m_tempoMap.back().bpm;
    }

    // Binary search for the interval
    auto it = std::upper_bound(m_tempoMap.begin(), m_tempoMap.end(), timeSec, 
        [](double val, const TempoPoint& p) {
            return val < p.timeSec;
        });

    auto p2 = *it;
    auto p1 = *(--it);

    // Linear interpolation
    double t = (timeSec - p1.timeSec) / (p2.timeSec - p1.timeSec);
    return p1.bpm + (p2.bpm - p1.bpm) * t;
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
