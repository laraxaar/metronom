#include "IMetronomeModule.h"
#include <iostream>
#include <random>

/**
 * @brief Automatically increases BPM every N measures.
 */
class BpmLadder : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    void processAudio(const float* input, float* output, uint32_t nFrames) override {}

    void onBeat(int beatIndex, uint32_t sampleOffset) override {}

    void onBar(int barIndex) override {
        m_barCounter++;
        if (m_barCounter >= m_measuresPerStep) {
            m_barCounter = 0;
            m_currentBpm += m_bpmIncrement;
            std::cout << "[BpmLadder] Increasing BPM to " << m_currentBpm << std::endl;
        }
    }

    void onConfigChange(const std::string& jsonConfig) override {
        // Parse ladder settings
    }

    const char* getName() const override { return "BpmLadder"; }

private:
    int m_barCounter = 0;
    int m_measuresPerStep = 4;
    double m_bpmIncrement = 5.0;
    double m_currentBpm = 120.0;
};

/**
 * @brief Randomly mutes the click to train internal timing.
 */
class RandomSilence : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    
    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        if (m_isSilent) {
            // Silence the buffer (assuming this is called after click synthesis)
            for (uint32_t i = 0; i < nFrames * 2; ++i) {
                output[i] = 0.0f;
            }
        }
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        m_isSilent = (dist(m_gen) < m_probability);
        if (m_isSilent) {
            std::cout << "[RandomSilence] Muting next beat" << std::endl;
        }
    }

    void onBar(int barIndex) override {}

    void onConfigChange(const std::string& jsonConfig) override {
        // Parse probability
    }

    const char* getName() const override { return "RandomSilence"; }

private:
    float m_probability = 0.2f;
    bool m_isSilent = false;
    std::mt19937 m_gen{std::random_device{}()};
};

/**
 * @brief Measures timing drift after the click stops.
 */
class HumanMetronomeTest : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        // Similar to AccuracyCalculator but specifically for 'blind' phase
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        m_beatCount++;
        if (m_beatCount >= m_activeBeats) {
            m_isMuted = true;
            if (m_beatCount == m_activeBeats) {
                std::cout << "[HumanMetronome] Click STOPPED. Can you keep the beat?" << std::endl;
            }
        }
        if (m_beatCount >= (m_activeBeats + m_testBeats)) {
            m_beatCount = 0;
            m_isMuted = false;
            std::cout << "[HumanMetronome] Click RESUMED." << std::endl;
        }
    }

    void onBar(int barIndex) override {}

    void onConfigChange(const std::string& jsonConfig) override {}

    const char* getName() const override { return "HumanMetronomeTest"; }

private:
    int m_beatCount = 0;
    int m_activeBeats = 16;
    int m_testBeats = 16;
    bool m_isMuted = false;
};
