#include "TempoCoach.h"
#include <string>

void TempoCoach::reset(double bpm) {
    m_targetBpm = bpm;
    totalHits = perfectHits = earlyHits = lateHits = missHits = 0;
    m_recentDevs.clear();
}

void TempoCoach::recordHit(double deviationMs) {
    totalHits++;
    double a = std::abs(deviationMs);

    if (a < 10) {
        perfectHits++;
    } else if (deviationMs < 0) {
        earlyHits++;
    } else {
        lateHits++;
    }

    if (a >= 40) missHits++;

    m_recentDevs.push_back(deviationMs);
    if (m_recentDevs.size() > m_windowSize)
        m_recentDevs.pop_front();
}

float TempoCoach::getAccuracyPercent() const {
    if (totalHits == 0) return 0.0f;
    float score = (float)perfectHits * 1.0f + (float)(earlyHits + lateHits) * 0.5f - (float)missHits * 0.5f;
    return std::max(0.0f, (score / totalHits) * 100.0f);
}

std::string TempoCoach::getScoreRank() const {
    float acc = getAccuracyPercent();
    if (acc > 98.0f) return "S+";
    if (acc > 95.0f) return "S";
    if (acc > 90.0f) return "A";
    if (acc > 80.0f) return "B";
    if (acc > 70.0f) return "C";
    return "D";
}

GrooveProfile TempoCoach::getGrooveProfile() const {
    GrooveProfile profile;
    if (m_recentDevs.empty()) return profile;

    double sum = 0;
    for (double d : m_recentDevs) sum += d;
    profile.earlyTendencyMs = (float)(sum / m_recentDevs.size());

    // Stability: based on variance
    double variance = 0;
    double mean = sum / m_recentDevs.size();
    for (double d : m_recentDevs) variance += (d - mean) * (d - mean);
    variance /= m_recentDevs.size();
    
    profile.stabilityPercent = std::max(0.0f, 100.0f - (float)std::sqrt(variance) * 2.0f);
    
    return profile;
}
