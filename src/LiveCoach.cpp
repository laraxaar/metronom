#include "LiveCoach.h"
#include <algorithm>
#include <cmath>

void LiveCoach::reset() {
    m_state = State::Neutral;
    m_holdHits = 0;
    m_bestStabilityEarly = 0.0f;
    m_lockedBaseline = false;
}

LiveCoach::Output LiveCoach::update(double meanDevMs, double jitterMs, float playerStabilityPercent, double elapsedSec) {
    Output out;

    // Capture baseline stability in first 2 minutes
    if (elapsedSec < 120.0) {
        m_bestStabilityEarly = std::max(m_bestStabilityEarly, playerStabilityPercent);
    } else if (!m_lockedBaseline) {
        // lock once after early window
        m_lockedBaseline = true;
        if (m_bestStabilityEarly <= 0.0f) m_bestStabilityEarly = playerStabilityPercent;
    }

    const bool haveData = std::isfinite(meanDevMs) && std::isfinite(jitterMs);
    const double absMean = std::abs(meanDevMs);

    // Thresholds (tuned conservative to avoid "yelling")
    const double rushDragThr = 10.0;  // ms mean bias
    const double pocketMeanThr = 3.0; // ms mean
    const double pocketJitThr = 5.0;  // ms jitter

    const bool rushing = haveData && meanDevMs <= -rushDragThr;
    const bool dragging = haveData && meanDevMs >= rushDragThr;
    const bool inPocket = haveData && absMean <= pocketMeanThr && jitterMs <= pocketJitThr;

    const bool relax = (elapsedSec >= 300.0) && m_lockedBaseline
        && (m_bestStabilityEarly >= 85.0f)
        && (playerStabilityPercent <= m_bestStabilityEarly - 15.0f);

    // State transitions with hysteresis (must persist for a few updates)
    auto enter = [&](State s, int hold) {
        m_state = s;
        m_holdHits = hold;
    };
    if (m_holdHits > 0) --m_holdHits;

    if (relax && m_state != State::Relax) {
        enter(State::Relax, 24);
    } else if (inPocket) {
        // Prefer pocket if stable
        if (m_state != State::InPocket) enter(State::InPocket, 16);
    } else if (rushing) {
        if (m_state != State::Rushing) enter(State::Rushing, 16);
    } else if (dragging) {
        if (m_state != State::Dragging) enter(State::Dragging, 16);
    } else if (m_holdHits == 0) {
        m_state = State::Neutral;
    }

    out.state = m_state;
    out.flow = (m_state == State::InPocket);

    switch (m_state) {
        case State::Rushing:
            out.message = "You are rushing! Calm down, breathe. Focus on the kick drum.";
            break;
        case State::Dragging:
            out.message = "You are dragging... Stay relaxed. Keep your attack consistent.";
            break;
        case State::InPocket:
            out.message = "Consistent! In the Pocket.";
            break;
        case State::Relax:
            out.message = "Check your shoulders. Relax your plucking hand.";
            break;
        case State::Neutral:
        default:
            out.message = "";
            break;
    }

    return out;
}

