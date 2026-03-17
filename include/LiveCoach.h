#pragma once
#include <cstdint>
#include <string>

/**
 * @brief Live coaching state machine (trend-based feedback).
 *
 * Input signals:
 *  - mean deviation (ms) over last 8..16 hits
 *  - jitter (stddev ms) over last 8..16 hits
 *  - player stability percent (0..100) from LiveTempoDetector
 *  - elapsed seconds
 *
 * Output:
 *  - a short message + "flow" indicator
 */
class LiveCoach {
public:
    enum class State : uint8_t {
        Neutral = 0,
        Rushing,
        Dragging,
        InPocket,
        Relax
    };

    struct Output {
        const char* message = "";
        State state = State::Neutral;
        bool flow = false;
    };

    void reset();

    /**
     * @brief Update coaching output.
     *
     * @param meanDevMs mean deviation (negative=early, positive=late)
     * @param jitterMs std deviation of deviation
     * @param playerStabilityPercent 0..100
     * @param elapsedSec seconds since session start
     */
    Output update(double meanDevMs, double jitterMs, float playerStabilityPercent, double elapsedSec);

private:
    State m_state = State::Neutral;
    int m_holdHits = 0; // hysteresis counter (hit-based)

    // For 5+ minute "relax" detection
    float m_bestStabilityEarly = 0.0f;
    bool m_lockedBaseline = false;
};

