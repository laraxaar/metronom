#include "FreePlayTracker.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
void FreePlayTracker::reset() {
    m_onsetTimes.clear();
    liveBpm.store(0.0);
    stability.store(100.0);
    avgBpm.store(0.0);
    driftDirection.store(0);
    totalHits.store(0);
}

void FreePlayTracker::recordOnset(double tsSec) {
    m_onsetTimes.push_back(tsSec);
    totalHits.fetch_add(1);

    // Trim old onsets
    while (!m_onsetTimes.empty() && (tsSec - m_onsetTimes.front()) > m_windowSec)
        m_onsetTimes.pop_front();
    while (m_onsetTimes.size() > m_maxOnsets)
        m_onsetTimes.pop_front();

    if (m_onsetTimes.size() < 2) return;

    updateStats(tsSec);
}

void FreePlayTracker::updateStats(double tsSec) {
    std::vector<double> intervals;
    for (size_t i = 1; i < m_onsetTimes.size(); ++i) {
        double gap = m_onsetTimes[i] - m_onsetTimes[i-1];
        if (gap > 0.1 && gap < 3.0) // 20-600 BPM
            intervals.push_back(gap);
    }
    if (intervals.empty()) return;

    // Responsive live BPM (last 4 intervals)
    int recentN = std::min((int)intervals.size(), 4);
    double recentSum = 0;
    for (int i = (int)intervals.size() - recentN; i < (int)intervals.size(); ++i)
        recentSum += intervals[i];
    liveBpm.store(60.0 / (recentSum / recentN));

    // Overall stability and average
    double total = std::accumulate(intervals.begin(), intervals.end(), 0.0);
    double meanIvl = total / intervals.size();
    avgBpm.store(60.0 / meanIvl);

    double sqSum = 0;
    for (auto x : intervals) sqSum += (x - meanIvl) * (x - meanIvl);
    double cv = std::sqrt(sqSum / intervals.size()) / std::max(0.001, meanIvl);
    stability.store(std::max(0.0, 100.0 - cv * 300.0));

    // Drift detection
    if (intervals.size() >= 6) {
        int half = (int)intervals.size() / 2;
        double firstAvg = 0, secondAvg = 0;
        for (int i = 0; i < half; ++i) firstAvg += intervals[i];
        for (int i = half; i < (int)intervals.size(); ++i) secondAvg += intervals[i];
        firstAvg /= half;
        secondAvg /= (intervals.size() - half);
        double diffPct = (secondAvg - firstAvg) / firstAvg * 100.0;
        if (diffPct < -3.0) driftDirection.store(1); // Speeding up
        else if (diffPct > 3.0) driftDirection.store(-1); // Slowing down
        else driftDirection.store(0);
    }
}
