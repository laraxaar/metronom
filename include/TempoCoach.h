#pragma once
#include <deque>
#include <atomic>
#include <cmath>
#include <algorithm>

/**
 * @brief Analyzes hit accuracy to provide coaching advice.
 * Refactored from legacy engi.cpp.
 */
class TempoCoach {
public:
    TempoCoach() = default;

    void reset(double bpm);
    void recordHit(double deviationMs);

    // Stats
    int totalHits = 0;
    int perfectHits = 0; // <10ms
    int goodHits = 0;    // <25ms
    int okHits = 0;      // <40ms
    int missHits = 0;    // >=40ms
    double sumDeviation = 0;
    double avgDeviationMs = 0;

    std::vector<double> getRecentDeviations() const {
        return std::vector<double>(m_recentDevs.begin(), m_recentDevs.end());
    }
    
    // 0=can increase, 1=keep, 2=decrease
    std::atomic<int> adviceLevel{1};

private:
    double m_targetBpm = 0;
    std::deque<double> m_recentDevs;
    const int m_windowSize = 32;

    void computeAdvice();
};
