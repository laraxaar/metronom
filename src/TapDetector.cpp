#include "TapDetector.h"

void TapDetector::tap(double tsSec) {
    m_timestamps.push_back(tsSec);
    while (!m_timestamps.empty() && (tsSec - m_timestamps.front()) > m_windowSec)
        m_timestamps.pop_front();
    if (m_timestamps.size() > m_bufferSize)
        m_timestamps.pop_front();
}

double TapDetector::getBpm() const {
    if (m_timestamps.size() < 2) return 0.0;
    double sum = 0.0;
    for (size_t i = 1; i < m_timestamps.size(); ++i)
        sum += m_timestamps[i] - m_timestamps[i - 1];
    double avg = sum / (m_timestamps.size() - 1);
    return avg > 0 ? 60.0 / avg : 0.0;
}

double TapDetector::getStability() const {
    if (m_timestamps.size() < 3) return 100.0;
    std::vector<double> intervals;
    for (size_t i = 1; i < m_timestamps.size(); ++i)
        intervals.push_back(m_timestamps[i] - m_timestamps[i - 1]);
    double mean = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
    double sqSum = 0.0;
    for (auto x : intervals) sqSum += (x - mean) * (x - mean);
    double stdDev = std::sqrt(sqSum / intervals.size());
    return std::max(0.0, 100.0 - stdDev * 500.0);
}
