#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>

class TempoMap;
class ITrainingModule;

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
    enum class GridId : uint8_t { A = 0, B = 1 };

    struct Event {
        uint32_t sampleOffset = 0;  ///< sample index inside current block
        uint16_t stepIndex = 0;     ///< step index within bar [0..stepsPerBar-1]
        GridId grid = GridId::A;    ///< which grid emitted this event
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

    // ----- Grid B (polyrhythm) -----
    void setGridBEnabled(bool enabled) { m_gridBEnabled.store(enabled, std::memory_order_relaxed); }
    bool isGridBEnabled() const { return m_gridBEnabled.load(std::memory_order_relaxed); }
    void setSubdivisionPartsB(int partsPerQuarter); // 1,2,3,4,5,7
    int getSubdivisionPartsB() const { return m_partsPerQuarterB.load(std::memory_order_relaxed); }
    int getStepsPerBarB() const { return m_stepsPerBarB.load(std::memory_order_relaxed); }
    int getCurrentStepB() const { return m_currentStepB.load(std::memory_order_relaxed); }
    void setGridBClicksPerBar(int clicks); // polyrhythm: X clicks across one bar
    int getGridBClicksPerBar() const { return m_gridBClicksPerBar.load(std::memory_order_relaxed); }

    // ----- Grid editing (thread-safe) -----
    void setStepState(int stepIndex, StepState state);
    StepState getStepState(int stepIndex) const;
    void cycleStepState(int stepIndex); // ACCENT -> NORMAL -> MUTE -> ACCENT

    void setStepStateB(int stepIndex, StepState state);
    StepState getStepStateB(int stepIndex) const;
    void cycleStepStateB(int stepIndex);

    // ----- Transport -----
    void reset();                        ///< resets phase + playhead to 0
    int getCurrentStep() const { return m_currentStep.load(std::memory_order_relaxed); }

    // ----- Tempo map (thread-safe) -----
    /**
     * @brief Set or clear the tempo map (nullptr = disabled).
     *
     * This method is safe to call from a UI/control thread while audio is running.
     */
    void setTempoMap(std::shared_ptr<const TempoMap> map);

    /**
     * @brief Global practice scaling factor, multiplies tempo-map BPM.
     *
     * Typical values: 0.25, 0.5, 1.0.
     * Clamp range: 0.01..4.0 (safety).
     */
    void setScaling(float factor);

    float getScaling() const { return m_scaling.load(std::memory_order_relaxed); }

    // ----- Training modules (RT-safe chain, pointers must outlive audio) -----
    static constexpr int MAX_TRAINING_MODULES = 8;
    void clearTrainingModules();
    bool addTrainingModule(ITrainingModule* module);

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

    // Tempo map + scaling
    std::atomic<float> m_scaling{1.0f};
    std::shared_ptr<const TempoMap> m_tempoMap; // accessed via atomic_load/store free functions
    uint64_t m_totalSamples = 0;                // transport time (audio-thread only)

    // Training modules
    ITrainingModule* m_trainingModules[MAX_TRAINING_MODULES]{};
    std::atomic<int> m_trainingModuleCount{0};

    // Grid state
    std::atomic<uint8_t>  m_stepStates[MAX_STEPS]{}; // StepState as uint8_t
    std::atomic<int>      m_currentStep{0};
    std::atomic<uint8_t>  m_stepStatesB[MAX_STEPS]{};
    std::atomic<int>      m_currentStepB{0};
    std::atomic<int>      m_partsPerQuarterB{1};
    std::atomic<int>      m_stepsPerBarB{4};
    std::atomic<bool>     m_gridBEnabled{false};
    std::atomic<int>      m_gridBClicksPerBar{0}; // 0 = use partsPerQuarterB timing

    // Sample-accurate phase accumulator
    double m_samplesUntilNextTick = 0.0; // can be negative when catching up
    double m_samplesUntilNextTickB = 0.0;
};
