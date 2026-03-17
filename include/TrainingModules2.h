#pragma once
#include "ITrainingModule.h"
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace training {

// Small RT-safe PRNG (xorshift32)
struct XorShift32 {
    uint32_t s = 0x12345678u;
    uint32_t nextU32() {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }
    float next01() {
        // 24-bit mantissa
        return static_cast<float>(nextU32() & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }
    float nextSigned1() { return next01() * 2.0f - 1.0f; }
};

/**
 * @brief BPM Ladder: increases BPM every N bars.
 *
 * The module reports a tempo override via getBpmOverride().
 */
class BpmLadder final : public ITrainingModule {
public:
    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setMeasuresPerStep(int n) { m_measuresPerStep.store(std::max(1, n), std::memory_order_relaxed); }
    void setBpmIncrement(double inc) { m_bpmIncrement.store(inc, std::memory_order_relaxed); }
    void reset(double baseBpm) {
        m_currentBpm.store(baseBpm, std::memory_order_relaxed);
        m_barCounter = 0;
    }

    void modifyNextStep(Step& step, double& /*offsetMs*/) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        if (!step.isDownbeat) return;

        const int nBars = m_measuresPerStep.load(std::memory_order_relaxed);
        if (nBars <= 0) return;

        ++m_barCounter;
        if (m_barCounter >= nBars) {
            m_barCounter = 0;
            const double inc = m_bpmIncrement.load(std::memory_order_relaxed);
            double next = m_currentBpm.load(std::memory_order_relaxed) + inc;
            if (!std::isfinite(next)) next = 120.0;
            next = std::clamp(next, 10.0, 1000.0);
            m_currentBpm.store(next, std::memory_order_relaxed);
        }
    }

    double getBpmOverride() const override {
        if (!m_enabled.load(std::memory_order_relaxed)) return -1.0;
        return m_currentBpm.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool>   m_enabled{true};
    std::atomic<int>    m_measuresPerStep{4};
    std::atomic<double> m_bpmIncrement{5.0};
    std::atomic<double> m_currentBpm{120.0};

    // audio-thread state
    int m_barCounter = 0;
};

/**
 * @brief RandomSilence: probabilistically mutes individual steps.
 */
class RandomSilence final : public ITrainingModule {
public:
    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setProbability(float p) { m_probability.store(std::clamp(p, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setSeed(uint32_t seed) { if (seed != 0) m_rng.s = seed; }

    void modifyNextStep(Step& step, double& /*offsetMs*/) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        const float p = m_probability.load(std::memory_order_relaxed);
        if (p <= 0.0f) return;
        if (m_rng.next01() < p) {
            step.state = StepState::MUTE;
            step.velocity = 0.0f;
        }
    }

private:
    std::atomic<bool>  m_enabled{true};
    std::atomic<float> m_probability{0.2f};
    XorShift32 m_rng{};
};

/**
 * @brief GrooveShift: adds micro-timing offsets (human drummer feel).
 *
 * Offset is applied in milliseconds: positive = late, negative = early.
 */
class GrooveShift final : public ITrainingModule {
public:
    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setMaxShiftMs(float ms) { m_maxShiftMs.store(std::max(0.0f, ms), std::memory_order_relaxed); }
    void setSeed(uint32_t seed) { if (seed != 0) m_rng.s = seed; }

    void modifyNextStep(Step& /*step*/, double& offsetMs) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        const float maxMs = m_maxShiftMs.load(std::memory_order_relaxed);
        if (maxMs <= 0.0f) return;
        offsetMs += static_cast<double>(m_rng.nextSigned1() * maxMs);
    }

private:
    std::atomic<bool>  m_enabled{true};
    std::atomic<float> m_maxShiftMs{0.0f}; // 0 = disabled by default
    XorShift32 m_rng{};
};

/**
 * @brief Disappearing: alternates visible/hidden bars.
 *
 * During hidden bars, all steps are muted.
 */
class Disappearing final : public ITrainingModule {
public:
    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setVisibleBars(int n) { m_visibleBars.store(std::max(1, n), std::memory_order_relaxed); }
    void setHiddenBars(int n) { m_hiddenBars.store(std::max(1, n), std::memory_order_relaxed); }
    void reset() { m_inHidden = false; m_barInPhase = 0; }

    void modifyNextStep(Step& step, double& /*offsetMs*/) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;

        if (step.isDownbeat) {
            const int vis = m_visibleBars.load(std::memory_order_relaxed);
            const int hid = m_hiddenBars.load(std::memory_order_relaxed);
            const int limit = m_inHidden ? hid : vis;

            ++m_barInPhase;
            if (limit > 0 && m_barInPhase >= limit) {
                m_barInPhase = 0;
                m_inHidden = !m_inHidden;
            }
        }

        if (m_inHidden) {
            step.state = StepState::MUTE;
            step.velocity = 0.0f;
        }
    }

private:
    std::atomic<bool> m_enabled{true};
    std::atomic<int>  m_visibleBars{4};
    std::atomic<int>  m_hiddenBars{4};

    // audio-thread state
    bool m_inHidden = false;
    int  m_barInPhase = 0;
};

/**
 * @brief Drunken Drummer: unstable timing + humanized velocity with "fatigue".
 *
 * - Time drift: random offset in ±(5..15) ms scaled by level.
 * - Velocity humanization: random gain + fatigue (every 4th beat slightly quieter).
 */
class DrunkenDrummer final : public ITrainingModule {
public:
    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setLevel(float level) { m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setSeed(uint32_t seed) { if (seed != 0) m_rng.s = seed; }

    void modifyNextStep(Step& step, double& offsetMs) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        const float lvl = m_level.load(std::memory_order_relaxed);
        if (lvl <= 0.0001f) return;

        // Update drift targets on beat starts (more "musical" changes).
        if (step.isBeatStart) {
            // 5..15ms range scaled by level
            const float baseMs = 5.0f + 10.0f * lvl;
            // Occasionally change target strongly; otherwise small wander
            const float jumpP = 0.10f + 0.25f * lvl;
            if (m_rng.next01() < jumpP) {
                m_targetOffsetMs = m_rng.nextSigned1() * baseMs;
                m_targetGainMod = m_rng.nextSigned1() * (0.18f * lvl); // +/- 18% at full
            } else {
                // gentle wander
                m_targetOffsetMs += m_rng.nextSigned1() * (0.8f * lvl);
                m_targetGainMod += m_rng.nextSigned1() * (0.02f * lvl);
                m_targetOffsetMs = std::clamp(m_targetOffsetMs, -baseMs, baseMs);
                m_targetGainMod = std::clamp(m_targetGainMod, -0.25f * lvl, 0.25f * lvl);
            }
        }

        // Inertia: critically damped-ish approach (simple 1-pole smoothing per tick event)
        // Higher level => slower recovery + larger wander (feels "drunk").
        const float alpha = 0.18f - 0.10f * lvl; // 0.08..0.18
        m_curOffsetMs += (m_targetOffsetMs - m_curOffsetMs) * alpha;
        m_curGainMod += (m_targetGainMod - m_curGainMod) * alpha;

        offsetMs += static_cast<double>(m_curOffsetMs);

        // Fatigue: every 4th BEAT start tends to drop a bit.
        float fatigue = 1.0f;
        if (step.isBeatStart && (step.beatIndex % 4) == 3) {
            fatigue -= 0.08f + 0.22f * lvl; // 0.08..0.30
            if (fatigue < 0.0f) fatigue = 0.0f;
        }

        // Accents sometimes overshoot.
        float accentOver = 1.0f;
        if (step.state == StepState::ACCENT) {
            const float p = 0.05f + 0.15f * lvl;
            if (m_rng.next01() < p) accentOver += 0.10f + 0.25f * lvl;
        }

        const float gain = std::clamp(1.0f + m_curGainMod, 0.2f, 1.8f);
        step.velocity = std::clamp(step.velocity * gain * fatigue * accentOver, 0.0f, 1.2f);
        if (step.velocity <= 0.0001f) {
            step.state = StepState::MUTE;
            step.velocity = 0.0f;
        }
    }

private:
    std::atomic<bool>  m_enabled{false};
    std::atomic<float> m_level{0.0f};
    XorShift32 m_rng{};

    // audio-thread drift state (inertia)
    float m_targetOffsetMs = 0.0f;
    float m_curOffsetMs = 0.0f;
    float m_targetGainMod = 0.0f;
    float m_curGainMod = 0.0f;
};

/**
 * @brief Rhythm Boss: survival mode FSM with periodic "attacks".
 *
 * Attacks:
 *  - MutePulse: mute one bar (also intended as "flashbang")
 *  - TempoJump: +/-10% tempo jump for one bar
 *  - AccentShift: move strong beat to another beat for one bar
 */
class RhythmBoss final : public ITrainingModule {
public:
    enum class Attack : uint8_t { None = 0, MutePulse = 1, TempoJump = 2, AccentShift = 3 };

    void setEnabled(bool e) { m_enabled.store(e, std::memory_order_relaxed); }
    void setLevel(float level) { m_level.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setBaseBpm(double bpm) { m_baseBpm.store(std::clamp(bpm, 10.0, 1000.0), std::memory_order_relaxed); }
    void setFlashTarget(std::atomic<float>* flash) { m_flash.store(flash, std::memory_order_relaxed); }
    void reset() {
        m_currentAttack = Attack::None;
        m_attackBarsLeft = 0;
        m_barsUntilNext = 8;
        m_overrideBpm.store(-1.0, std::memory_order_relaxed);
    }

    void modifyNextStep(Step& step, double& /*offsetMs*/) override {
        if (!m_enabled.load(std::memory_order_relaxed)) return;

        if (step.isDownbeat) {
            // Tick bar counters
            if (m_attackBarsLeft > 0) {
                --m_attackBarsLeft;
                if (m_attackBarsLeft == 0) {
                    m_currentAttack = Attack::None;
                    m_overrideBpm.store(-1.0, std::memory_order_relaxed);
                }
            } else {
                --m_barsUntilNext;
                if (m_barsUntilNext <= 0) {
                    startRandomAttack(step.stepsPerBar);
                }
            }
        }

        // Apply attack effects for this bar
        switch (m_currentAttack) {
            case Attack::MutePulse:
                step.state = StepState::MUTE;
                step.velocity = 0.0f;
                break;

            case Attack::TempoJump:
                // no per-step changes here (tempo handled via override)
                break;

            case Attack::AccentShift:
                if (step.isBeatStart) {
                    // Move strong beat to target beat (beatIndex)
                    if (static_cast<int>(step.beatIndex) == m_accentTargetBeat) {
                        step.state = StepState::ACCENT;
                        step.velocity = 1.0f;
                    } else if (step.state == StepState::ACCENT) {
                        // demote original accents while attack active
                        step.state = StepState::NORMAL;
                        step.velocity = 0.6f;
                    }
                }
                break;

            case Attack::None:
            default:
                break;
        }
    }

    double getBpmOverride() const override {
        if (!m_enabled.load(std::memory_order_relaxed)) return -1.0;
        return m_overrideBpm.load(std::memory_order_relaxed);
    }

private:
    void startRandomAttack(int stepsPerBar) {
        const float lvl = m_level.load(std::memory_order_relaxed);

        // Choose next interval: 8..16 bars (slightly shorter at high level)
        int minBars = 8;
        int maxBars = 16;
        if (lvl > 0.6f) { minBars = 8; maxBars = 12; }
        m_barsUntilNext = minBars + static_cast<int>(m_rng.next01() * (maxBars - minBars + 1));

        // Choose attack type
        const float r = m_rng.next01();
        if (r < 0.34f) m_currentAttack = Attack::MutePulse;
        else if (r < 0.67f) m_currentAttack = Attack::TempoJump;
        else m_currentAttack = Attack::AccentShift;

        m_attackBarsLeft = 1; // one-bar attacks for now

        // Flashbang signal
        if (m_currentAttack == Attack::MutePulse) {
            if (auto* f = m_flash.load(std::memory_order_relaxed)) {
                f->store(1.0f, std::memory_order_relaxed);
            }
        }

        if (m_currentAttack == Attack::TempoJump) {
            const double base = m_baseBpm.load(std::memory_order_relaxed);
            const float sign = (m_rng.next01() < 0.5f) ? -1.0f : 1.0f;
            const double jump = 1.0 + static_cast<double>(sign) * 0.10; // +/- 10%
            m_overrideBpm.store(std::clamp(base * jump, 10.0, 1000.0), std::memory_order_relaxed);
        }

        if (m_currentAttack == Attack::AccentShift) {
            // Choose a beat target within bar: 1..beats-1 (avoid downbeat)
            // stepsPerBar = beatsPerBar * partsPerQuarter(A). Approx beats = stepsPerBar / partsPerQuarter.
            int beatsApprox = 4;
            if (stepsPerBar > 0) beatsApprox = std::max(2, std::min(16, stepsPerBar)); // safe fallback
            // we actually get beatIndex from engine; choose 1..7
            m_accentTargetBeat = 1 + (m_rng.nextU32() % 3); // small variety default
            if (auto* f = m_flash.load(std::memory_order_relaxed)) {
                f->store(0.7f, std::memory_order_relaxed);
            }
        }
    }

    std::atomic<bool>  m_enabled{false};
    std::atomic<float> m_level{0.5f};
    std::atomic<double> m_baseBpm{120.0};
    std::atomic<double> m_overrideBpm{-1.0};
    std::atomic<std::atomic<float>*> m_flash{nullptr};

    // audio-thread state
    XorShift32 m_rng{};
    Attack m_currentAttack{Attack::None};
    int m_attackBarsLeft = 0;
    int m_barsUntilNext = 8;
    int m_accentTargetBeat = 1;
};

} // namespace training

