#include "IMetronomeModule.h"
#include "LockFreeQueue.h"
#include "MetronomeCore.h"
#include "PreciseTuner.h"
#include "UIController.h"
#include "SmartOnsetDetector.h"
#include "FreePlayTracker.h"
#include "TapDetector.h"
#include "TempoCoach.h"
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

    // Legacy getters for UI
    float getInputPeak() const { return m_inputPeak.load(); }
    double getLiveBpm() const { return m_freePlay.liveBpm.load(); }
    double getStability() const { return m_freePlay.stability.load(); }

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
    
    // UI Bridge
    std::shared_ptr<UIController> m_uiController;
    
    // Plugin chain
    std::vector<std::unique_ptr<IMetronomeModule>> m_modules;
    
    // Thread-safe state
    std::atomic<bool> m_running{false};
    std::atomic<double> m_currentBpm{120.0};
    std::atomic<float> m_inputPeak{0.0f};
    std::atomic<bool> m_analyzing{false};
    
    // Device management (internal abstraction)
    struct DeviceImpl;
    std::unique_ptr<DeviceImpl> m_device;

    // Helper for sample processing
    void synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets);
    void processInputAnalysis(const float* input, uint32_t nFrames);
};
