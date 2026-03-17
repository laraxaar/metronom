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
    int currentBeatIndex;            // Current active beat
    std::vector<int> beatPattern;    // Current beat pattern (for visualization)
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

    /**
     * @brief Thread-safe custom tuning update.
     */
    bool postCustomTuning(const std::vector<float>& frequencies) {
        return m_tuningQueue.push(frequencies);
    }
    
    std::optional<std::vector<float>> getLatestCustomTuning() {
        std::optional<std::vector<float>> latest = std::nullopt;
        while (auto opt = m_tuningQueue.pop()) {
            latest = opt;
        }
        return latest;
    }

    bool postActiveModule(int moduleType) {
        return m_moduleQueue.push(moduleType);
    }

    std::optional<int> getLatestActiveModule() {
        std::optional<int> latest = std::nullopt;
        while (auto opt = m_moduleQueue.pop()) {
            latest = opt;
        }
        return latest;
    }

    struct ModuleParam {
        int id;
        float value;
    };

    bool postModuleParam(int id, float value) {
        return m_paramQueue.push({id, value});
    }

    std::optional<ModuleParam> getNextModuleParam() {
        return m_paramQueue.pop();
    }

private:
    LockFreeQueue<VisualData> m_visualQueue;
    LockFreeQueue<std::vector<float>> m_tuningQueue{4}; // Small queue for tuning changes
    LockFreeQueue<int> m_moduleQueue{4}; // Enum for which module to load (0=None, 1=Ladder, 2=Silence, 3=Human, 4=RhythmBoss)
    LockFreeQueue<ModuleParam> m_paramQueue{16};
    
    std::atomic<double> m_targetBpm{120.0};
    std::atomic<bool> m_isMuted{false};
};
