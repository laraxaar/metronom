#include "IMetronomeModule.h"
#include "LockFreeQueue.h"
#include "MetronomeCore.h"
#include "PreciseTuner.h"
#include "UIController.h"
#include "SmartOnsetDetector.h"
#include "FreePlayTracker.h"
#include "TapDetector.h"
#include "TempoCoach.h"
#include "PolyrhythmEngine.h"
#include "MidiSyncManager.h"
#include <vector>
#include <memory>
#include <atomic>
#include <string>

/**
 * @brief The main host for the metronome system.
 * Manages audio I/O, timing, and plugin execution.
 */
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    /**
     * @brief Initialize the engine with a JSON configuration file.
     */
    bool initialize(const std::string& configPath);

    /**
     * @brief Start the audio processing stream.
     */
    bool start();

    /**
     * @brief Stop the audio processing stream.
     */
    void stop();

    /**
     * @brief Primary audio callback (called by the hardware driver).
     */
    void audioCallback(const float* input, float* output, uint32_t nFrames);

    /**
     * @brief Dynamically add a module to the processing chain.
     */
    void addModule(std::unique_ptr<IMetronomeModule> module);

    /**
     * @brief Update the metronome tempo from any thread.
     */
    void setBpm(double bpm);

    /**
     * @brief Set the UI controller for state synchronization.
     */
    void setUIController(std::shared_ptr<UIController> uiController) { m_uiController = uiController; }

    /**
     * @brief Add a polyrythmic pattern.
     */
    void addPolyrhythm(int ratio);

    /**
     * @brief Set the tuner mode and instrument optimizations.
     */
    void setInstrumentMode(PreciseTuner::Mode mode);

    /**
     * @brief Apply a one-shot timing shift to the next click.
     */
    void setGrooveShift(double delayMs);

    /**
     * @brief Set per-beat accent pattern.
     */
    void setBeatPattern(const std::vector<int>& pattern);

    /**
     * @brief Enable/disable audio monitoring.
     */
    void setMonitoring(bool enabled, float gain = 1.0f);

    // Legacy getters for UI
    float getInputPeak() const { return m_inputPeak.load(); }
    float getOutputPeak() const { return m_outputPeak.load(); }
    double getLiveBpm() const { return m_freePlay.liveBpm.load(); }
    double getStability() const { return m_freePlay.stability.load(); }
    float getCpuLoad() const { return m_cpuLoad.load(); }

private:
    // Audio configuration
    AudioParams m_params;
    
    // Core timing engine
    MetronomeCore m_metronomeCore;
    
    // Advanced DSP & Legacy Components
    PreciseTuner m_tuner;
    SmartOnsetDetector m_onset;
    FreePlayTracker m_freePlay;
    TapDetector m_tap;
    TempoCoach m_coach;
    PolyrhythmEngine m_polyEngine;
    MidiSyncManager m_midiSync;
    
    // UI Bridge
    std::shared_ptr<UIController> m_uiController;
    
    // Plugin chain
    std::vector<std::unique_ptr<IMetronomeModule>> m_modules;
    
    // Thread-safe state
    std::atomic<bool> m_running{false};
    std::atomic<double> m_currentBpm{120.0};
    std::atomic<uint64_t> m_totalFrames{0};
    
    std::atomic<float> m_inputPeak{0.0f};
    std::atomic<float> m_outputPeak{0.0f};
    std::atomic<float> m_cpuLoad{0.0f};
    
    std::atomic<bool> m_monitoring{false};
    std::atomic<float> m_monitorGain{1.0f};
    std::atomic<int64_t> m_grooveShiftFrames{0};
    
    std::atomic<bool> m_analyzing{false};
    
    std::vector<int> m_beatPattern;
    std::mutex m_patternMutex;
    
    // Device management (internal abstraction)
    struct DeviceImpl;
    std::unique_ptr<DeviceImpl> m_device;

    // Helper for sample processing
    void synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets);
    void processInputAnalysis(const float* input, uint32_t nFrames);
};
