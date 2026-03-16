#include "TempoCoach.h"

void TempoCoach::reset(double bpm) {
    m_targetBpm = bpm;
    totalHits = perfectHits = goodHits = okHits = missHits = 0;
    sumDeviation = avgDeviationMs = 0;
    adviceLevel.store(1);
    m_recentDevs.clear();
}

void TempoCoach::recordHit(double deviationMs) {
    double a = std::abs(deviationMs);
    totalHits++;
    sumDeviation += a;
    avgDeviationMs = sumDeviation / totalHits;

    if      (a < 10)  perfectHits++;
    else if (a < 25)  goodHits++;
    else if (a < 40)  okHits++;
    else              missHits++;

    m_recentDevs.push_back(a);
    while (m_recentDevs.size() > m_windowSize)
        m_recentDevs.pop_front();

    computeAdvice();
}

void TempoCoach::computeAdvice() {
    if (totalHits < 8) { adviceLevel.store(1); return; }

    double perfectPct = (double)perfectHits / totalHits * 100.0;
    double missPct    = (double)missHits / totalHits * 100.0;

    // Can increase tempo: >60% perfect, <5% misses
    if (perfectPct > 60.0 && missPct < 5.0 && avgDeviationMs < 15.0) {
        adviceLevel.store(0);
    }
    // Should decrease tempo: >30% misses or avg deviation > 45ms
    else if (missPct > 30.0 || avgDeviationMs > 45.0) {
        adviceLevel.store(2);
    }
    else {
        adviceLevel.store(1);
    }
}
