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
     * @param outBeatIndices Vector to store indices of detected beats (e.g., 0, 1, 2, 3 in 4/4).
     */
    void process(uint32_t nFrames, uint32_t sampleRate, std::vector<uint32_t>& outBeatOffsets, std::vector<int>& outBeatIndices);

    /**
     * @brief Set the time signature top (number of beats in a bar).
     */
    void setTimeSignature(int top) { m_timeSigTop.store(top, std::memory_order_relaxed); }
    
    /**
     * @brief Get current beat index (within time signature).
     */
    int getCurrentBeatIndex() const { return m_currentBeatIndex; }
    double getTotalElapsedSec() const { return m_totalElapsedSec; }

    /**
     * @brief Reset the metronome state.
     */
    void reset();

    /**
     * @brief Add a point or ramp to the tempo map.
     */
    void addTempoPoint(double timeStart, double timeEnd, double bpmStart, double bpmEnd);

    /**
     * @brief Get BPM at a specific time using linear interpolation.
     */
    double getBpmAtTime(double timeSec) const;

    /**
     * @brief Set the groove intensity (0.0 to 1.0).
     */
    void setGroove(float intensity) { m_grooveIntensity.store(intensity, std::memory_order_relaxed); }

    /**
     * @brief Clear all points from the tempo map.
     */
    void clearTempoMap() { m_tempoMap.clear(); }

private:
    std::atomic<double> m_targetBpm{120.0};
    std::atomic<int> m_subdivision{1};
    std::atomic<int> m_timeSigTop{4};
    std::atomic<float> m_grooveIntensity{0.0f};
    
    double m_currentPhase = 0.0;      // 0.0 to 1.0 within a beat
    double m_totalElapsedSec = 0.0;
    int m_currentBeatIndex = 0;       // Track which beat we are on (0..top-1)
    double m_nextBeatThreshold = 1.0; // Phase threshold for next beat (allows for groove/jitter)
    
    std::vector<TempoPoint> m_tempoMap;
};
