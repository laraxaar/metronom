#pragma once
#include "MixerState.h"
#include "MetronomeEngine.h"
#include <cstdint>
#include <vector>
#include <string>

/**
 * @brief Final audio summing mixer.
 *
 * Channels:
 *  - Master (post sum)
 *  - Click (normal)
 *  - Accent
 *  - Input monitoring (bass)
 *
 * Responsibilities:
 *  - Mix click events (sample-accurate) using loaded WAV samples or fallback synth
 *  - Mix polyrhythm triggers (optional samples)
 *  - Apply monitoring gain
 *  - Compute per-channel peak/RMS meters for UI (writes into MixerState)
 */
class AudioMixer {
public:
    enum class SampleId : int {
        ClickNormal = 0,
        ClickAccent = 1,
        Kick = 2,
        HiHat = 3,
        Cowbell = 4
    };

    explicit AudioMixer(MixerState& state);

    void setSampleRate(uint32_t sr);
    uint32_t getSampleRate() const { return m_sampleRate; }

    // Sample loading (control thread)
    bool loadWavSample(SampleId id, const std::string& path);
    void clearSample(SampleId id);

    // Processing (audio thread)
    void processBlock(
        const float* inputInterleaved,
        uint32_t numInputChannels,
        float* outputInterleaved,
        uint32_t numOutputChannels,
        uint32_t nFrames,
        const MetronomeEngine::Event* tickEvents,
        size_t numTickEvents,
        const std::vector<std::pair<uint32_t, std::vector<float>>>& polyTriggers,
        double bpmForFallbackSynth
    );

private:
    MixerState& m_state;
    uint32_t m_sampleRate = 48000;

    struct SampleData {
        std::vector<float> mono; // mono samples at file SR (no resample yet)
        bool loaded = false;
    };
    SampleData m_samples[5];

    // Tail buffers (mono) to avoid cutting off samples between blocks (RT-safe)
    struct TailBuffer {
        std::vector<float> data; // scaled samples to drain
        uint32_t len = 0;        // valid length
        uint32_t pos = 0;        // read position
        void clear() { len = 0; pos = 0; }
        uint32_t available() const { return (len > pos) ? (len - pos) : 0; }
    };
    TailBuffer m_clickTail;
    TailBuffer m_polyTail;
    uint32_t m_tailCapacity = 0; // max sample length reserved (control thread)

    void mixOneShotSample(
        const std::vector<float>& mono,
        uint32_t offset,
        float* out,
        uint32_t ch,
        uint32_t nFrames,
        float gain,
        TailBuffer& tail
    );

    // Meter helpers
    static void computePeakRmsMono(const float* x, uint32_t n, float& outPeak, float& outRms);
    static float smoothPeak(float prev, float next);
};

