#pragma once
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
#include "InputProcessor.h"
#include "DeviceManager.h"
#include "PitchResult.h"
#include "TunerWorker.h"
#include "RhythmGrid.h"
#include "MixerState.h"
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include <mutex>

// Forward declaration for RtAudio
class RtAudio;

/**
 * @brief The main host for the metronome/tuner system.
 * 
 * Uses RtAudio for low-latency duplex audio I/O.
 * Integrates InputProcessor (DC filter + RingBuffer) for bass-optimized input,
 * DeviceManager for API-separated device enumeration with channel selection,
 * and PitchResult for lock-free pitch data transfer.
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
     * @brief Primary audio callback (called by the RtAudio driver).
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
     * @brief Removes all custom training modules.
     */
    void clearModules();

    /**
     * @brief Add a polyrythmic pattern.
     */
    void addPolyrhythm(int ratio);

    /**
     * @brief Set the tuner mode and instrument optimizations.
     */
    void setInstrumentMode(PreciseTuner::Mode mode);

    /**
     * @brief Set custom string frequencies for the tuner.
     */
    void setCustomTuning(const std::vector<float>& frequencies);

    /**
     * @brief Apply a one-shot timing shift to the next click.
     */
    void setGrooveShift(double delayMs);

    /**
     * @brief Set per-beat accent pattern.
     */
    void setBeatPattern(const std::vector<int>& pattern);

    // --- RhythmGrid ---
    RhythmGrid& getGrid() { return m_grid; }
    const RhythmGrid& getGrid() const { return m_grid; }
    void cycleGridStep(int stepIndex) { m_grid.cycleStep(stepIndex); }
    void setGridSubdivision(int subdiv);
    GridSnapshot getGridSnapshot() const { return m_grid.getSnapshot(); }

    // --- Mixer ---
    MixerState& getMixer() { return m_mixer; }
    const MixerState& getMixer() const { return m_mixer; }

    /**
     * @brief Enable/disable audio monitoring.
     */
    void setMonitoring(bool enabled, float gain = 1.0f);

    /**
     * @brief Enable/disable the metronome click.
     */
    void setClickEnabled(bool enabled) { m_clickEnabled.store(enabled); }
    bool isClickEnabled() const { return m_clickEnabled.load(); }

    /**
     * @brief Set the rhythmic subdivision.
     */
    void setSubdivision(int sub) { m_metronomeCore.setSubdivision(sub); }
    
    /**
     * @brief Set the time signature top (number of beats in a bar).
     */
    void setTimeSignature(int top) { m_metronomeCore.setTimeSignature(top); }

    /**
     * @brief Set the groove intensity.
     */
    void setGroove(float intensity) { m_metronomeCore.setGroove(intensity); }

    /**
     * @brief Add a point to the tempo map.
     */
    void addTempoPoint(double timeStart, double timeEnd, double bpmStart, double bpmEnd) {
        m_metronomeCore.addTempoPoint(timeStart, timeEnd, bpmStart, bpmEnd);
    }

    /**
     * @brief Clear the tempo map.
     */
    void clearTempoMap() { m_metronomeCore.clearTempoMap(); }

    // =====================================================================
    // Device Management (RtAudio-based, API-separated)
    // =====================================================================

    /**
     * @brief Get the DeviceManager for full API-separated device enumeration.
     */
    DeviceManager& getDeviceManager() { return m_deviceManager; }
    const DeviceManager& getDeviceManager() const { return m_deviceManager; }

    /**
     * @brief Legacy device listing — now delegates to DeviceManager.
     */
    struct DeviceInfo {
        std::string name;
        int index;
        std::string apiName;
        uint32_t maxInputChannels{0};
        uint32_t maxOutputChannels{0};
        uint32_t preferredSampleRate{0};
    };
    std::vector<DeviceInfo> listPlaybackDevices();
    std::vector<DeviceInfo> listCaptureDevices();

    /**
     * @brief Set the input device by RtAudio device ID.
     * Takes effect on next initialize() or reopenStream().
     */
    void setInputDevice(int deviceId);

    /**
     * @brief Set the output device by RtAudio device ID.
     * Takes effect on next initialize() or reopenStream().
     */
    void setOutputDevice(int deviceId);

    /**
     * @brief Set the input channel index (0-based).
     * For example, if bass is plugged into channel 2 of a multi-channel interface,
     * call setInputChannel(1).
     */
    void setInputChannel(uint32_t channel);

    /**
     * @brief Set the desired sample rate.
     * Higher rates (48kHz+) improve pitch detection accuracy for low frequencies.
     */
    void setSampleRate(uint32_t sampleRate);

    /**
     * @brief Reopen the audio stream with current device/channel settings.
     * Call after changing input/output device or sample rate.
     */
    bool reopenStream();

    // =====================================================================
    // Free Play
    // =====================================================================
    void setFreePlayActive(bool active) { m_freePlay.active.store(active); }
    bool isFreePlayActive() const { return m_freePlay.active.load(); }
    void resetFreePlay() { m_freePlay.reset(); }
    double getFreePlayLiveBpm() const { return m_freePlay.liveBpm.load(); }
    double getFreePlayAvgBpm() const { return m_freePlay.avgBpm.load(); }
    double getFreePlayStability() const { return m_freePlay.stability.load(); }
    int getFreePlayDrift() const { return m_freePlay.driftDirection.load(); }
    int getFreePlayTotalHits() const { return m_freePlay.totalHits.load(); }

    // =====================================================================
    // Getters (thread-safe reads)
    // =====================================================================
    float getInputPeak() const { return m_inputProcessor.getPeakLevel(); }
    float getOutputPeak() const { return m_outputPeak.load(); }
    double getLiveBpm() const { return m_currentBpm.load(); }
    double getStability() const { return m_freePlay.stability.load(); }
    float getCpuLoad() const { return m_cpuLoad.load(); }
    float getAccuracy() const { return m_coach.getAccuracyPercent(); }
    std::string getScoreRank() const { return m_coach.getScoreRank(); }
    GrooveProfile getGrooveProfile() const { return m_coach.getGrooveProfile(); }
    float getInputRMS() const { return m_inputProcessor.getCurrentRMS(); }

    /**
     * @brief Get the shared PitchResult for reading detected pitch from GUI.
     */
    const PitchResult& getPitchResult() const { return m_inputProcessor.getPitchResult(); }

    /**
     * @brief Get the TunerResult with frequency, cents, note name, signal level.
     */
    const TunerResult& getTunerResult() const { return m_tuner.getResult(); }

    /**
     * @brief Get access to the InputProcessor for advanced analysis.
     */
    InputProcessor& getInputProcessor() { return m_inputProcessor; }

    /**
     * @brief Get the PreciseTuner for mode/preset configuration.
     */
    PreciseTuner& getTuner() { return m_tuner; }

    void setTuning(int instrument, int tuning);
    void setPracticeScale(float scale); // 0.25 to 1.0
    bool loadWavSample(int type, const std::string& path); // type: 0=normal, 1=accent

private:
    // Audio configuration
    AudioParams m_params;
    
    // Core timing engine
    MetronomeCore m_metronomeCore;
    
    // Input processing pipeline (DC filter + RingBuffer)
    InputProcessor m_inputProcessor;

    // Device management (RtAudio)
    DeviceManager m_deviceManager;
    
    // Advanced DSP & Legacy Components
    PreciseTuner m_tuner;
    TunerWorker m_tunerWorker;  ///< Dedicated thread for pitch analysis
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
    
    std::atomic<float> m_outputPeak{0.0f};
    std::atomic<float> m_cpuLoad{0.0f};
    
    std::atomic<bool> m_monitoring{false};
    std::atomic<float> m_monitorGain{1.0f};
    std::atomic<int64_t> m_grooveShiftFrames{0};
    
    std::atomic<bool> m_analyzing{false};
    std::atomic<bool> m_clickEnabled{true};
    
    RhythmGrid m_grid;
    MixerState m_mixer;
    
    // Click tail buffer (to prevent cut-off clicks at buffer boundaries)
    std::vector<float> m_clickTail;
    uint32_t m_clickTailPos = 0;
    
    // RtAudio instance
    std::unique_ptr<RtAudio> m_rtAudio;

    // Device selection state
    int m_selectedInputDevice{-1};   // -1 = use default
    int m_selectedOutputDevice{-1};  // -1 = use default 
    uint32_t m_requestedSampleRate{48000};

    float m_practiceScale = 1.0f;
    std::vector<float> m_normalSample;
    std::vector<float> m_accentSample;

    // Helper for sample processing
    void synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets, const std::vector<int>& indices);
    void processInputAnalysis(const float* input, uint32_t nFrames);
};
