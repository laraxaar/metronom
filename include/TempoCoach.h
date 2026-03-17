#pragma once
#include <deque>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

/**
 * @brief Analyzes hit accuracy to provide coaching advice.
 * Refactored from legacy engi.cpp.
 */
struct GrooveProfile {
    float earlyTendencyMs = 0.0f;
    float swingPercent = 0.0f;
    float stabilityPercent = 0.0f;
};

class TempoCoach {
public:
    TempoCoach() = default;

    void reset(double bpm);
    void recordHit(double deviationMs);

    // Stats
    int totalHits = 0;
    int perfectHits = 0; // <10ms
    int earlyHits = 0;
    int lateHits = 0;
    int missHits = 0;    // >=40ms
    
    float getAccuracyPercent() const;
    std::string getScoreRank() const; // S+, S, A, etc.
    GrooveProfile getGrooveProfile() const;
    const std::deque<double>& getRecentDeviations() const { return m_recentDevs; } 

private:
    double m_targetBpm = 0;
    std::deque<double> m_recentDevs;
    const int m_windowSize = 64;

    void computeAdvice();
};
