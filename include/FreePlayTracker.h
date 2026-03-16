#pragma once
#include <deque>
#include <atomic>
#include <numeric>
#include <cmath>
#include <algorithm>

/**
 * @brief Tracks inter-onset intervals to compute live BPM during free play.
 * Refactored from legacy engi.cpp.
 */
class FreePlayTracker {
public:
    FreePlayTracker() = default;

    void reset();

    /**
     * @brief Record a detected onset time.
     */
    void recordOnset(double tsSec);

    // Results (Atomic for lock-free reads)
    std::atomic<double> liveBpm{0.0};
    std::atomic<double> stability{100.0}; // 0-100%
    std::atomic<double> avgBpm{0.0};
    std::atomic<int> driftDirection{0}; // -1 slowing, 0 steady, +1 speeding
    std::atomic<int> totalHits{0};

    std::atomic<bool> active{false};

private:
    std::deque<double> m_onsetTimes;
    const int m_maxOnsets = 50;
    const double m_windowSec = 8.0;

    void updateStats(double tsSec);
};
