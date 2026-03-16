#pragma once
#include <deque>
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

/**
 * @brief Simple tap-tempo detector.
 * Refactored from legacy engi.cpp.
 */
class TapDetector {
public:
    void tap(double tsSec);
    double getBpm() const;
    double getStability() const;
    void reset() { m_timestamps.clear(); }

private:
    std::deque<double> m_timestamps;
    const int m_bufferSize = 10;
    const double m_windowSec = 4.0;
};
