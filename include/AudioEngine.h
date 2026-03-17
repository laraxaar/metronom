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
#include "MetronomeEngine.h"
#include "AccuracyAnalyzer.h"
#include "TrainingModules2.h"
#include "AudioMixer.h"
#include "LiveTempoDetector.h"
#include "LiveCoach.h"
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
    void setPolyrhythmEnabled(bool enabled);
    void setPolyrhythmRatio(int x, int y); // X against Y beats in bar
    void clearPolyrhythm();

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
    void cycleGridStep(int stepIndex);
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
    void setSubdivision(int sub) {
        m_metronomeCore.setSubdivision(sub);
        m_metronomeEngine.setSubdivisionParts(sub);
    }
    
    /**
     * @brief Set the time signature top (number of beats in a bar).
     */
    void setTimeSignature(int top) {
        m_metronomeCore.setTimeSignature(top);
        m_metronomeEngine.setBeatsPerBar(top);
    }

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
    void setBufferSize(uint32_t bufferFrames);

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
    double getLivePlayerBpm() const { return m_liveTempo.getLiveBpm(); }
    float getLivePlayerStability() const { return m_liveTempo.getStabilityPercent(); }
    // Live coaching (lock-free snapshot)
    const char* getLiveCoachText() const { return m_coachText; } // read via seq protocol below
    uint32_t getLiveCoachSeq() const { return m_coachSeq.load(std::memory_order_acquire); }
    bool isFlowActive() const { return m_flowActive.load(std::memory_order_relaxed); }

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
    bool loadMixerSample(int sampleId, const std::string& path); // 0=click,1=accent,2=kick,3=hat,4=cowbell

    // =====================================================================
    // Training (live, RT-safe)
    // =====================================================================
    enum class TrainingModuleId : int {
        Ladder = 1,
        RandomSilence = 2,
        Disappearing = 3,
        GrooveShift = 4,
        DrunkenDrummer = 5,
        RhythmBoss = 6
    };

    enum class TrainingParamId : int {
        LadderMeasuresPerStep = 100,
        LadderBpmIncrement = 101,
        SilenceProbability = 110,
        GrooveMaxShiftMs = 120,
        DisappearVisibleBars = 130,
        DisappearHiddenBars = 131,
        DrunkenLevel = 140,
        BossLevel = 150
    };

    void setTrainingEnabled(TrainingModuleId id, bool enabled);
    void adjustTrainingParam(TrainingParamId id, float delta);
    void setHumanTestEnabled(bool enabled);
    bool isHumanTestEnabled() const { return m_humanTestEnabled.load(std::memory_order_relaxed); }

    // UI getters (thread-safe)
    bool isTrainingLadderEnabled() const { return m_trainLadderEnabled.load(std::memory_order_relaxed); }
    bool isTrainingSilenceEnabled() const { return m_trainSilenceEnabled.load(std::memory_order_relaxed); }
    bool isTrainingDisappearingEnabled() const { return m_trainDisappearEnabled.load(std::memory_order_relaxed); }
    bool isTrainingGrooveEnabled() const { return m_trainGrooveEnabled.load(std::memory_order_relaxed); }
    bool isTrainingDrunkenEnabled() const { return m_trainDrunkenEnabled.load(std::memory_order_relaxed); }
    bool isTrainingBossEnabled() const { return m_trainBossEnabled.load(std::memory_order_relaxed); }
    float getTrainingSilenceProb() const { return m_silenceProb.load(std::memory_order_relaxed); }
    float getTrainingGrooveMaxShiftMs() const { return m_grooveMaxShiftMs.load(std::memory_order_relaxed); }
    int getTrainingLadderBars() const { return m_ladderBars.load(std::memory_order_relaxed); }
    float getTrainingLadderInc() const { return m_ladderInc.load(std::memory_order_relaxed); }
    int getTrainingDisappearVisible() const { return m_disappearVis.load(std::memory_order_relaxed); }
    int getTrainingDisappearHidden() const { return m_disappearHid.load(std::memory_order_relaxed); }
    float getTrainingDrunkenLevel() const { return m_drunkenLevel.load(std::memory_order_relaxed); }
    float getBossLevel() const { return m_bossLevel.load(std::memory_order_relaxed); }
    float getBossFlash() const { return m_bossFlash.load(std::memory_order_relaxed); }
    bool isBossGameOver() const { return m_bossGameOver.load(std::memory_order_relaxed); }

private:
    // Audio configuration
    AudioParams m_params;
    
    // Core timing engine
    MetronomeCore m_metronomeCore;

    // Sample-accurate rhythm engine (events + atomic grid)
    MetronomeEngine m_metronomeEngine;

    // Training module instances (live-configurable)
    training::BpmLadder     m_trainLadder;
    training::RandomSilence m_trainSilence;
    training::Disappearing  m_trainDisappear;
    training::GrooveShift   m_trainGroove;
    training::DrunkenDrummer m_trainDrunken;
    training::RhythmBoss     m_trainBoss;
    std::atomic<bool>       m_trainLadderEnabled{false};
    std::atomic<bool>       m_trainSilenceEnabled{false};
    std::atomic<bool>       m_trainDisappearEnabled{false};
    std::atomic<bool>       m_trainGrooveEnabled{false};
    std::atomic<bool>       m_trainDrunkenEnabled{false};
    std::atomic<bool>       m_trainBossEnabled{false};
    std::atomic<bool>       m_humanTestEnabled{false};
    std::atomic<float>      m_silenceProb{0.2f};
    std::atomic<float>      m_grooveMaxShiftMs{0.0f};
    std::atomic<int>        m_ladderBars{4};
    std::atomic<float>      m_ladderInc{5.0f};
    std::atomic<int>        m_disappearVis{4};
    std::atomic<int>        m_disappearHid{4};
    std::atomic<float>      m_drunkenLevel{0.0f};
    std::atomic<float>      m_bossLevel{0.5f};
    std::atomic<float>      m_bossFlash{0.0f}; // UI overlay intensity
    std::atomic<bool>       m_bossGameOver{false};
    uint64_t                m_bossStartFrame = 0;

    // Accuracy analyzer (onsets vs ticks)
    AccuracyAnalyzer m_accuracyAnalyzer;

    // Scratch buffer for onset detector (mono window, avoids allocations)
    std::vector<float> m_onsetWindow;

    // Final summing mixer
    AudioMixer m_audioMixer;
    
    // Input processing pipeline (DC filter + RingBuffer)
    InputProcessor m_inputProcessor;

    // Device management (RtAudio)
    DeviceManager m_deviceManager;
    
    // Advanced DSP & Legacy Components
    PreciseTuner m_tuner;
    TunerWorker m_tunerWorker;  ///< Dedicated thread for pitch analysis
    SmartOnsetDetector m_onset;
    FreePlayTracker m_freePlay;
    LiveTempoDetector m_liveTempo;
    LiveCoach m_liveCoach;

    // Live coach text (seqlock)
    std::atomic<uint32_t> m_coachSeq{0};
    char m_coachText[128] = {};
    std::atomic<bool> m_flowActive{false};
    TapDetector m_tap;
    TempoCoach m_coach;
    PolyrhythmEngine m_polyEngine;
    std::atomic<bool> m_polyEnabled{false};
    std::atomic<int> m_polyX{0};
    std::atomic<int> m_polyY{0};
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
    
    // RtAudio instance
    std::unique_ptr<RtAudio> m_rtAudio;

    // Device selection state
    int m_selectedInputDevice{-1};   // -1 = use default
    int m_selectedOutputDevice{-1};  // -1 = use default 
    uint32_t m_requestedSampleRate{48000};

    float m_practiceScale = 1.0f;
    // Helper for sample processing
    void processInputAnalysis(const float* input, uint32_t nFrames);
};
