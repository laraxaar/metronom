#pragma once
#include "IMetronomeModule.h"
#include "LockFreeQueue.h"
#include "MetronomeCore.h"
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
     * @param configPath Path to the JSON configuration file.
     * @return true on success.
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
     * @param input Buffer containing incoming audio data (e.g., from mic).
     * @param output Buffer to be filled with outgoing audio data (metronome click).
     * @param nFrames Number of samples in the current audio block.
     * @note This function MUST be real-time safe (no locks, no allocation).
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

private:
    // Audio configuration
    AudioParams m_params;
    
    // Core timing engine
    MetronomeCore m_metronomeCore;
    
    // Plugin chain
    std::vector<std::unique_ptr<IMetronomeModule>> m_modules;
    
    // Thread-safe state
    std::atomic<bool> m_running{false};
    std::atomic<double> m_currentBpm{120.0};
    
    // Device management (internal abstraction)
    struct DeviceImpl;
    std::unique_ptr<DeviceImpl> m_device;

    // Helper for sample processing
    void synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets);
};
