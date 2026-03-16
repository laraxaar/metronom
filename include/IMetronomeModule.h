#pragma once
#include <cstdint>
#include <string>

/**
 * @brief Configuration structure for the audio engine.
 */
struct AudioParams {
    uint32_t sampleRate;
    uint32_t bufferSize;
    uint32_t numInputChannels;
    uint32_t numOutputChannels;
};

/**
 * @brief Interface for metronome modules/plugins.
 * All modules must implement this to be loaded by the AudioEngine.
 */
class IMetronomeModule {
public:
    virtual ~IMetronomeModule() = default;

    /**
     * @brief Called once during module initialization.
     */
    virtual void onInitialize(const AudioParams& params) = 0;

    /**
     * @brief Main audio processing callback.
     * @note MUST be lock-free and memory-allocation free.
     */
    virtual void processAudio(const float* input, float* output, uint32_t nFrames) = 0;

    /**
     * @brief Triggered on every click/beat.
     * @param beatIndex The index of the beat in the current bar.
     * @param sampleOffset Offset in samples from the start of the current audio block.
     */
    virtual void onBeat(int beatIndex, uint32_t sampleOffset) = 0;

    /**
     * @brief Triggered at the start of a new measure.
     * @param barIndex The absolute index of the bar since start.
     */
    virtual void onBar(int barIndex) = 0;

    /**
     * @brief Update module configuration in real-time.
     * @param jsonConfig A JSON string containing module-specific settings.
     */
    virtual void onConfigChange(const std::string& jsonConfig) = 0;

    /**
     * @brief Interface name for plugin identification.
     */
    virtual const char* getName() const = 0;
};
