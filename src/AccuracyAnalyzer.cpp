#include "AccuracyAnalyzer.h"
#include "TempoCoach.h"
#include "MetronomeEngine.h"
#include <cmath>
#include <algorithm>

AccuracyAnalyzer::AccuracyAnalyzer(TempoCoach& coach) : m_coach(coach) {}

void AccuracyAnalyzer::setSampleRate(uint32_t sr) {
    if (sr == 0) sr = 48000;
    m_sampleRate = sr;
}

void AccuracyAnalyzer::reset() {
    m_tickWrite = 0;
    m_tickCount = 0;
    m_lastMatchedTick = 0;
}

void AccuracyAnalyzer::onTicks(uint64_t blockStartFrame, const MetronomeEngine::Event* events, size_t numEvents) {
    if (!m_enabled.load(std::memory_order_relaxed)) return;
    if (!events || numEvents == 0) return;

    for (size_t i = 0; i < numEvents; ++i) {
        const uint64_t absTick = blockStartFrame + static_cast<uint64_t>(events[i].sampleOffset);
        m_tickTimes[m_tickWrite] = absTick;
        m_tickWrite = (m_tickWrite + 1) % TICK_RING;
        if (m_tickCount < TICK_RING) ++m_tickCount;
    }
}

void AccuracyAnalyzer::onOnset(uint64_t onsetFrame) {
    if (!m_enabled.load(std::memory_order_relaxed)) return;
    if (m_tickCount == 0 || m_sampleRate == 0) return;

    // Search nearest tick among recent ticks.
    // We scan backwards from newest (better locality, small ring).
    uint64_t bestTick = 0;
    int64_t bestDiff = INT64_MAX;

    size_t idx = (m_tickWrite + TICK_RING - 1) % TICK_RING;
    for (size_t n = 0; n < m_tickCount; ++n) {
        const uint64_t t = m_tickTimes[idx];
        const int64_t diff = static_cast<int64_t>(onsetFrame) - static_cast<int64_t>(t);
        const int64_t ad = diff < 0 ? -diff : diff;
        if (ad < (bestDiff < 0 ? -bestDiff : bestDiff)) {
            bestDiff = diff;
            bestTick = t;
        }

        // Early exit: once ticks are far in the past, diffs will only grow.
        if (t + static_cast<uint64_t>(m_sampleRate) < onsetFrame && ad > (int64_t)m_sampleRate) {
            break;
        }

        idx = (idx + TICK_RING - 1) % TICK_RING;
    }

    if (bestTick == 0) return;
    if (bestTick == m_lastMatchedTick) return; // avoid double hits on same tick

    // Accept only if within 120ms window (typical human timing window)
    const double diffMs = (static_cast<double>(bestDiff) / static_cast<double>(m_sampleRate)) * 1000.0;
    if (std::abs(diffMs) > 120.0) return;

    m_lastMatchedTick = bestTick;
    m_coach.recordHit(diffMs);
}

