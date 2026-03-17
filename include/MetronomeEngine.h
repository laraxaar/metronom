#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

/**
 * @brief Real-time safe sample-accurate metronome + rhythm grid.
 *
 * - All timing is done by counting samples inside the audio callback.
 * - BPM is atomic and can be changed on the fly (10..1000).
 * - Grid step states are stored as atomics and can be edited live.
 * - Subdivision is expressed as "parts per quarter note": 1,2,3,4,5,7.
 */
class MetronomeEngine {
public:
    enum class StepState : uint8_t { ACCENT = 0, NORMAL = 1, MUTE = 2 };

    struct Event {
        uint32_t sampleOffset = 0;  ///< sample index inside current block
        uint16_t stepIndex = 0;     ///< step index within bar [0..stepsPerBar-1]
        StepState state = StepState::NORMAL;
        float velocity = 0.6f;      ///< 1.0 accent, 0.6 normal, 0.0 mute
    };

    static constexpr int MAX_STEPS = 256;   ///< max steps per bar (beatsPerBar * partsPerQuarter)
    static constexpr int MAX_EVENTS = 512;  ///< safety cap per block

    MetronomeEngine();

    // ----- Configuration (thread-safe) -----
    void setSampleRate(uint32_t sampleRate);
    void setBpm(double bpm);                    ///< clamps to 10..1000
    void setBeatsPerBar(int beatsPerBar);       ///< clamps to 1..64
    void setSubdivisionParts(int partsPerQuarter); ///< allowed: 1,2,3,4,5,7 (others coerced)

    double getBpm() const { return m_bpm.load(std::memory_order_relaxed); }
    int getBeatsPerBar() const { return m_beatsPerBar.load(std::memory_order_relaxed); }
    int getSubdivisionParts() const { return m_partsPerQuarter.load(std::memory_order_relaxed); }
    int getStepsPerBar() const { return m_stepsPerBar.load(std::memory_order_relaxed); }

    // ----- Grid editing (thread-safe) -----
    void setStepState(int stepIndex, StepState state);
    StepState getStepState(int stepIndex) const;
    void cycleStepState(int stepIndex); // ACCENT -> NORMAL -> MUTE -> ACCENT

    // ----- Transport -----
    void reset();                        ///< resets phase + playhead to 0
    int getCurrentStep() const { return m_currentStep.load(std::memory_order_relaxed); }

    /**
     * @brief Process one audio block and emit click trigger events.
     *
     * RT-safe: no locks, no allocations.
     *
     * @param nFrames number of frames in this block
     * @param outEvents pointer to caller-provided array
     * @param outCapacity max events that can be written
     * @return number of events written
     */
    size_t processBlock(uint32_t nFrames, Event* outEvents, size_t outCapacity);

private:
    static int coerceSubdivisionParts(int parts);
    void rebuildGridForNewSteps(int newStepsPerBar);

    static float stateToVelocity(StepState s) {
        switch (s) {
            case StepState::ACCENT: return 1.0f;
            case StepState::NORMAL: return 0.6f;
            case StepState::MUTE:   return 0.0f;
        }
        return 0.0f;
    }

    // Timing
    std::atomic<uint32_t> m_sampleRate{48000};
    std::atomic<double>   m_bpm{120.0};
    std::atomic<int>      m_beatsPerBar{4};
    std::atomic<int>      m_partsPerQuarter{1};  // 1,2,3,4,5,7
    std::atomic<int>      m_stepsPerBar{4};

    // Grid state
    std::atomic<uint8_t>  m_stepStates[MAX_STEPS]{}; // StepState as uint8_t
    std::atomic<int>      m_currentStep{0};

    // Sample-accurate phase accumulator
    double m_samplesUntilNextTick = 0.0; // can be negative when catching up
};

#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include <cstring>

/**
 * @brief State of a single rhythmic step.
 */
enum class StepType : uint8_t {
    ACCENT = 0,   ///< Loud click  (velocity 1.0)
    NORMAL = 1,   ///< Regular click (velocity 0.6)
    MUTE   = 2    ///< Silence      (velocity 0.0)
};

/**
 * @brief A click trigger event generated during processBlock.
 * Contains all info needed by the audio mixer to synthesize the correct sound.
 */
struct ClickEvent {
    uint32_t sampleOffset;  ///< Exact sample position within the current buffer
    int      stepIndex;     ///< Which step in the grid fired (0-based)
    int      beatIndex;     ///< Which beat within the bar (0-based, pre-subdivision)
    int      subIndex;      ///< Subdivision index within the beat (0-based)
    StepType type;          ///< Hit type: ACCENT, NORMAL, or MUTE
    float    velocity;      ///< 0.0–1.0
    bool     isDownbeat;    ///< True if this is beat 0 of the bar
};

/**
 * @brief Snapshot of engine state for lock-free UI transfer.
 */
struct EngineSnapshot {
    uint8_t  steps[64]{};      ///< StepType values
    int      numSteps = 0;
    int      currentStep = -1; ///< Playhead position (-1 = stopped)
    int      beatsPerBar = 4;
    int      subdivision = 1;
    double   bpm = 120.0;
    bool     running = false;
};

/**
 * @brief Sample-accurate Metronome Engine with Rhythm Grid.
 *
 * Replaces the phase-based MetronomeCore with precise sample counting.
 * All timing is computed in integer samples — zero accumulation drift.
 *
 * Features:
 *   - BPM 10–1000
 *   - Time signatures 1–32 beats per bar
 *   - Subdivisions: 1, 2, 3 (triplets), 4, 5, 7
 *   - Per-step ACCENT / NORMAL / MUTE
 *   - Click trigger events with sample-accurate offsets
 *   - Thread-safe BPM and grid changes via std::atomic
 *   - Uses sample rate from AudioEngine (passed at init)
 *
 * Integration:
 *   - Call processBlock() from the audio callback
 *   - Read outEvents for click triggers to feed into the audio mixer
 *   - Grid mutations (cycleStep, setSubdivision) are safe from any thread
 */
class MetronomeEngine {
public:
    static constexpr int MAX_STEPS = 64;
    static constexpr int MAX_EVENTS_PER_BLOCK = 64;

    MetronomeEngine();
    ~MetronomeEngine() = default;

    // =====================================================================
    // Initialization
    // =====================================================================

    /**
     * @brief Set the sample rate. Must be called before processBlock.
     */
    void setSampleRate(uint32_t sampleRate);

    /**
     * @brief Reset all counters and playhead to zero.
     */
    void reset();

    // =====================================================================
    // Parameters (thread-safe, can be called from any thread)
    // ===============