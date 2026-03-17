#pragma once
#include <cstdint>
#include <atomic>
#include <cstddef>

class TempoCoach;
#include "MetronomeEngine.h"

/**
 * @brief Compares detected onsets against metronome tick times.
 *
 * Audio thread usage:
 * - call onTicks() once per audio block (even if click is muted)
 * - call onOnset() when an onset is detected
 *
 * Thread-safety:
 * - onTicks/onOnset are audio-thread only
 * - enable flags can be toggled from any thread (atomics)
 */
class AccuracyAnalyzer {
public:
    explicit AccuracyAnalyzer(TempoCoach& coach);

    void setSampleRate(uint32_t sr);
    void reset();

    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

    /**
     * @brief Human metronome test mode: metronome can be silent, but analysis continues.
     * When enabled, analyzer will keep scoring even if click is off.
     */
    void setHumanTestEnabled(bool enabled) { m_humanTest.store(enabled, std::memory_order_relaxed); }
    bool isHumanTestEnabled() const { return m_humanTest.load(std::memory_order_relaxed); }

    // Call from audio callback
    void onTicks(uint64_t blockStartFrame, const MetronomeEngine::Event* events, size_t numEvents);
    void onOnset(uint64_t onsetFrame);

private:
    TempoCoach& m_coach;
    uint32_t m_sampleRate = 48000;

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_humanTest{false};

    // Ring buffer of recent tick times (absolute sample frames)
    static constexpr size_t TICK_RING = 512;
    uint64_t m_tickTimes[TICK_RING]{};
    size_t m_tickWrite = 0;
    size_t m_tickCount = 0;

    uint64_t m_lastMatchedTick = 0;
};

