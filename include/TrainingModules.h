#pragma once
#include "IMetronomeModule.h"
#include <random>
#include <iostream>

class BpmLadder : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    void processAudio(const float* input, float* output, uint32_t nFrames) override {}

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        // ...
    }

    void onBar(int barIndex) override {
        m_barCounter++;
        if (m_barCounter >= m_measuresPerStep) {
            m_barCounter = 0;
            m_currentBpm += m_bpmIncrement;
            std::cout << "[BpmLadder] Increasing BPM to " << m_currentBpm << std::endl;
        }
    }

    void onConfigChange(const std::string& jsonConfig) override {}
    void setParameter(int id, float value) override {
        if (id == 0) m_measuresPerStep = static_cast<int>(value);
        if (id == 1) m_bpmIncrement = value;
    }
    const char* getName() const override { return "BpmLadder"; }
    double getBpmOverride() const override { return m_currentBpm; }
    void reset(double baseBpm) { m_currentBpm = baseBpm; m_barCounter = 0; }

private:
    int m_barCounter = 0;
    int m_measuresPerStep = 4;
    double m_bpmIncrement = 5.0;
    double m_currentBpm = 120.0;
};

class RandomSilence : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    
    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        if (m_isSilent) {
            for (uint32_t i = 0; i < nFrames * 2; ++i) {
                output[i] = 0.0f;
            }
        }
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        m_isSilent = (dist(m_gen) < m_probability);
    }

    void onBar(int barIndex) override {}
    void onConfigChange(const std::string& jsonConfig) override {}
    void setParameter(int id, float value) override {
        if (id == 0) m_probability = value;
    }
    const char* getName() const override { return "RandomSilence"; }

    void setProbability(float p) { m_probability = p; }

private:
    float m_probability = 0.2f;
    bool m_isSilent = false;
    std::mt19937 m_gen{std::random_device{}()};
};

class HumanMetronomeTest : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        if (m_isMuted) {
            for (uint32_t i = 0; i < nFrames * 2; ++i) output[i] = 0.0f;
        }
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        m_beatCount++;
        if (m_beatCount >= m_activeBeats) {
            m_isMuted = true;
        }
        if (m_beatCount >= (m_activeBeats + m_testBeats)) {
            m_beatCount = 0;
            m_isMuted = false;
        }
    }

    void onBar(int barIndex) override {}
    void onConfigChange(const std::string& jsonConfig) override {}
    void setParameter(int id, float value) override {
        if (id == 0) m_activeBeats = static_cast<int>(value);
        if (id == 1) m_testBeats = static_cast<int>(value);
    }
    const char* getName() const override { return "HumanMetronomeTest"; }

private:
    int m_beatCount = 0;
    int m_activeBeats = 16;
    int m_testBeats = 16;
    bool m_isMuted = false;
};

class RhythmBoss : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {}
    
    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        if (m_isSilent) {
            for (uint32_t i = 0; i < nFrames * 2; ++i) output[i] = 0.0f;
        }
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        m_tickCount++;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        m_isSilent = (dist(m_gen) < 0.03f * m_level);
        
        if (m_level >= 3 && m_tickCount % 16 == 0) {
            std::uniform_int_distribution<int> jitterDist(-5 * m_level, 5 * m_level);
            m_jitter = (double)jitterDist(m_gen);
        } else {
            m_jitter = 0.0;
        }
    }

    void onBar(int barIndex) override {}
    void onConfigChange(const std::string& jsonConfig) override {}
    void setParameter(int id, float value) override {
        if (id == 0) m_level = static_cast<int>(value);
    }
    const char* getName() const override { return "RhythmBoss"; }
    
    double getBpmOverride() const override { 
        if (m_jitter != 0.0) return 120.0 + m_jitter; // Simplified, should probably use current BPM
        return -1.0; 
    }

private:
    int m_level = 5;
    int m_tickCount = 0;
    bool m_isSilent = false;
    double m_jitter = 0.0;
    std::mt19937 m_gen{std::random_device{}()};
};
