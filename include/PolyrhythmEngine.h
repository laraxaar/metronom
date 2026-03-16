#pragma once
#include <vector>
#include <cstdint>
#include <memory>

/**
 * @brief Independent voice for polyrhythmic patterns.
 */
struct PolyVoice {
    int ratio;
    double bpm;
    int bpmTop;
    double nextFrame;
    double intervalFrames;
    uint32_t clicksRemaining;
    std::vector<float> sample;
    bool active = false;
};

/**
 * @brief Handles multiple simultaneous rhythmic patterns.
 * Refactored from legacy engi.cpp.
 */
class PolyrhythmEngine {
public:
    PolyrhythmEngine() = default;

    /**
     * @brief Add a polyrhythmic voice.
     * @param ratio Number of clicks in the measure.
     * @param masterBpm Current master BPM.
     * @param bpmTop Time signature numerator.
     * @param currentFrame Current absolute frame count.
     * @param sample Optional custom sample (if empty, uses default tone).
     */
    void addVoice(int ratio, double masterBpm, int bpmTop, uint64_t currentFrame, const std::vector<float>& sample, uint32_t sampleRate);

    /**
     * @brief Process voices and find trigger offsets.
     */
    void process(uint32_t nFrames, uint64_t startFrame, std::vector<std::pair<uint32_t, std::vector<float>>>& outTriggers);

    void clear() { m_voices.clear(); }

private:
    std::vector<PolyVoice> m_voices;
};
