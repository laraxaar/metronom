#pragma once
#include <cstdint>
#include <vector>
#include <atomic>

/**
 * @brief Represents a point in the tempo map for BPM interpolation.
 */
struct TempoPoint {
    double timeStart;
    double timeEnd;
    double bpmStart;
    double bpmEnd;
};

/**
 * @brief Core timing logic for the metronome.
 * Handles sample-accurate beat calculation and tempo map interpolation.
 * Designed to be used inside the audio callback.
 */
class MetronomeCore {
public:
    MetronomeCore() = default;

    /**
     * @brief Reset the metronome state.
     */
    void reset();

    /**
     * @brief Set the target BPM.
     */
    void setBpm(double bpm) { m_targetBpm.store(bpm, std::memory_order_relaxed); }

    /**
     * @brief Set the rhythmic subdivision (1=quarter, 2=eighth, 4=sixteenth, etc.)
     */
    void setSubdivision(int subdivision) { m_subdivision.store(subdivision, std::memory_order_relaxed); }

    /**
     * @brief Calculate beat onsets for the current audio block.
     * @param nFrames Number of samples in the current block.
     * @param sampleRate The hardware sample rate.
     * @param outBeatOffsets Vector to store sample offsets of detected beats in this block.
     */
    void process(uint32_t nFrames, uint32_t sampleRate, std::vector<uint32_t>& outBeatOffsets);

    /**
     * @brief Add a point or ramp to the tempo map.
     */
    void addTempoPoint(double timeStart, double timeEnd, double bpmStart, double bpmEnd);

    /**
     * @brief Get BPM at a specific time using linear interpolation.
     */
    double getBpmAtTime(double timeSec) const;

private:
    std::atomic<double> m_targetBpm{120.0};
    std::atomic<int> m_subdivision{1};
    
    double m_currentPhase = 0.0;      // 0.0 to 1.0 within a beat
    double m_totalElapsedSec = 0.0;
    
    std::vector<TempoPoint> m_tempoMap;
};
