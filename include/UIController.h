#pragma once
#include "LockFreeQueue.h"
#include <vector>
#include <atomic>
#include <memory>

/**
 * @brief Structure for passing waveform data to the UI.
 */
struct VisualData {
    std::vector<float> waveform;
    std::vector<float> spectrum;
    float peakInput;
    float peakOutput;
    float currentFrequency;
    float confidence;
    float cpuLoad;
    std::vector<float> hitDeviations; // New field for Visual Analytics
};

/**
 * @brief Manages communication between the AudioEngine and the GUI.
 */
class UIController {
public:
    UIController(size_t queueCapacity = 16);
    ~UIController() = default;

    /**
     * @brief Post new visual data from the audio thread.
     * @note MUST be real-time safe.
     */
    bool postVisualData(const VisualData& data);

    /**
     * @brief Retrieve the latest visual data for the GUI thread.
     */
    std::optional<VisualData> getLatestVisualData();

    /**
     * @brief Thread-safe parameter updates.
     */
    void setBpm(double bpm) { m_targetBpm.store(bpm); }
    double getBpm() const { return m_targetBpm.load(); }

    void setMuted(bool muted) { m_isMuted.store(muted); }
    bool isMuted() const { return m_isMuted.load(); }

private:
    LockFreeQueue<VisualData> m_visualQueue;
    
    std::atomic<double> m_targetBpm{120.0};
    std::atomic<bool> m_isMuted{false};
};
